/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "Utils.hpp"

#include "dbus-sensor_config.h"

#include "DeviceMgmt.hpp"
#include "VariantVisitors.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

using powerStatePair = std::pair<std::string, bool>;
using powerMatchPair =
    std::pair<std::string, std::unique_ptr<sdbusplus::bus::match_t>>;

static bool manufacturingMode = false;
static std::vector<powerStatePair> powerStatusOn;
static std::vector<powerStatePair> biosHasPost;
static std::vector<powerStatePair> chassisStatusOn;

static std::unique_ptr<sdbusplus::bus::match_t> powerMatch = nullptr;
static std::unique_ptr<sdbusplus::bus::match_t> postMatch = nullptr;
static std::unique_ptr<sdbusplus::bus::match_t> chassisMatch = nullptr;
static std::vector<powerMatchPair> powerMatchVec;
static std::vector<powerMatchPair> postMatchVec;
static std::vector<powerMatchPair> chassisMatchVec;
static std::vector<std::shared_ptr<boost::asio::steady_timer>> timerVec;
static std::vector<std::shared_ptr<boost::asio::steady_timer>>
    timerChassisStatusVec;

/**
 * return the contents of a file
 * @param[in] hwmonFile - the path to the file to read
 * @return the contents of the file as a string or nullopt if the file could not
 * be opened.
 */

std::optional<std::string> openAndRead(const std::string& hwmonFile)
{
    std::string fileVal;
    std::ifstream fileStream(hwmonFile);
    if (!fileStream.is_open())
    {
        return std::nullopt;
    }
    std::getline(fileStream, fileVal);
    return fileVal;
}

/**
 * given a hwmon temperature base name if valid return the full path else
 * nullopt
 * @param[in] directory - the hwmon sysfs directory
 * @param[in] permitSet - a set of labels or hwmon basenames to permit. If this
 * is empty then *everything* is permitted.
 * @return a string to the full path of the file to create a temp sensor with or
 * nullopt to indicate that no sensor should be created for this basename.
 */
std::optional<std::string> getFullHwmonFilePath(
    const std::string& directory, const std::string& hwmonBaseName,
    const std::set<std::string>& permitSet)
{
    std::optional<std::string> result;
    std::string filename;
    if (permitSet.empty())
    {
        result = directory + "/" + hwmonBaseName + "_input";
        return result;
    }
    filename = directory + "/" + hwmonBaseName + "_label";
    auto searchVal = openAndRead(filename);
    if (!searchVal)
    {
        /* if the hwmon temp doesn't have a corresponding label file
         * then use the hwmon temperature base name
         */
        searchVal = hwmonBaseName;
    }
    if (permitSet.find(*searchVal) != permitSet.end())
    {
        result = directory + "/" + hwmonBaseName + "_input";
    }
    return result;
}

/**
 * retrieve a set of basenames and labels to allow sensor creation for.
 * @param[in] config - a map representing the configuration for a specific
 * device
 * @return a set of basenames and labels to allow sensor creation for. An empty
 * set indicates that everything is permitted.
 */
std::set<std::string> getPermitSet(const SensorBaseConfigMap& config)
{
    auto permitAttribute = config.find("Labels");
    std::set<std::string> permitSet;
    if (permitAttribute != config.end())
    {
        try
        {
            auto val =
                std::get<std::vector<std::string>>(permitAttribute->second);

            permitSet.insert(std::make_move_iterator(val.begin()),
                             std::make_move_iterator(val.end()));
        }
        catch (const std::bad_variant_access& err)
        {
            std::cerr << err.what()
                      << ":PermitList does not contain a list, wrong "
                         "variant type.\n";
        }
    }
    return permitSet;
}

bool getSensorConfiguration(
    const std::string& type,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    ManagedObjectType& resp, bool useCache)
{
    static ManagedObjectType managedObj;
    std::string typeIntf = configInterfaceName(type);

    if (!useCache)
    {
        managedObj.clear();
        sdbusplus::message_t getManagedObjects =
            dbusConnection->new_method_call(
                entityManagerName, "/xyz/openbmc_project/inventory",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        try
        {
            sdbusplus::message_t reply =
                dbusConnection->call(getManagedObjects);
            reply.read(managedObj);
        }
        catch (const sdbusplus::exception_t& e)
        {
            std::cerr << "While calling GetManagedObjects on service:"
                      << entityManagerName << " exception name:" << e.name()
                      << "and description:" << e.description()
                      << " was thrown\n";
            return false;
        }
    }
    for (const auto& pathPair : managedObj)
    {
        for (const auto& [intf, cfg] : pathPair.second)
        {
            if (intf.starts_with(typeIntf))
            {
                resp.emplace(pathPair);
                break;
            }
        }
    }
    return true;
}

bool findFiles(const fs::path& dirPath, std::string_view matchString,
               std::vector<fs::path>& foundPaths, int symlinkDepth)
{
    std::error_code ec;
    if (!fs::exists(dirPath, ec))
    {
        return false;
    }

    std::vector<std::regex> matchPieces;

    size_t pos = 0;
    std::string token;
    // Generate the regex expressions list from the match we were given
    while ((pos = matchString.find('/')) != std::string::npos)
    {
        token = matchString.substr(0, pos);
        matchPieces.emplace_back(token);
        matchString.remove_prefix(pos + 1);
    }
    matchPieces.emplace_back(std::string{matchString});

    // Check if the match string contains directories, and skip the match of
    // subdirectory if not
    if (matchPieces.size() <= 1)
    {
        std::regex search(std::string{matchString});
        std::smatch match;
        for (auto p = fs::recursive_directory_iterator(
                 dirPath, fs::directory_options::follow_directory_symlink);
             p != fs::recursive_directory_iterator(); ++p)
        {
            std::string path = p->path().string();
            if (!is_directory(*p))
            {
                if (std::regex_search(path, match, search))
                {
                    foundPaths.emplace_back(p->path());
                }
            }
            if (p.depth() >= symlinkDepth)
            {
                p.disable_recursion_pending();
            }
        }
        return true;
    }

    // The match string contains directories, verify each level of sub
    // directories
    for (auto p = fs::recursive_directory_iterator(
             dirPath, fs::directory_options::follow_directory_symlink);
         p != fs::recursive_directory_iterator(); ++p)
    {
        std::vector<std::regex>::iterator matchPiece = matchPieces.begin();
        fs::path::iterator pathIt = p->path().begin();
        for (const fs::path& dir : dirPath)
        {
            if (dir.empty())
            {
                // When the path ends with '/', it gets am empty path
                // skip such case.
                break;
            }
            pathIt++;
        }

        while (pathIt != p->path().end())
        {
            // Found a path deeper than match.
            if (matchPiece == matchPieces.end())
            {
                p.disable_recursion_pending();
                break;
            }
            std::smatch match;
            std::string component = pathIt->string();
            std::regex regexPiece(*matchPiece);
            if (!std::regex_match(component, match, regexPiece))
            {
                // path prefix doesn't match, no need to iterate further
                p.disable_recursion_pending();
                break;
            }
            matchPiece++;
            pathIt++;
        }

        if (!is_directory(*p))
        {
            if (matchPiece == matchPieces.end())
            {
                foundPaths.emplace_back(p->path());
            }
        }

        if (p.depth() >= symlinkDepth)
        {
            p.disable_recursion_pending();
        }
    }
    return true;
}

std::vector<powerStatePair>::iterator findPowerStateByPath(
    const std::string& path, std::vector<powerStatePair>& powerStatus)
{
    return std::find_if(
        powerStatus.begin(), powerStatus.end(),
        [&path](const auto& pair) { return pair.first == std::string(path); });
}

std::vector<powerMatchPair>::iterator findPowerMatchByPath(
    const std::string& path, std::vector<powerMatchPair>& powerMatch)
{
    return std::find_if(
        powerMatch.begin(), powerMatch.end(),
        [&path](const auto& pair) { return pair.first == std::string(path); });
}

bool isPowerOn(const size_t& slotId)
{
    std::string path = power::path + std::to_string(slotId);
    auto match_it = findPowerMatchByPath(path, powerMatchVec);
    if (match_it == powerMatchVec.end())
    {
        throw std::runtime_error("Power Match Not Created");
    }
    auto it = findPowerStateByPath(path, powerStatusOn);
    if (it == powerStatusOn.end())
    {
        return false;
    }

    return it->second;
}

bool hasBiosPost(const size_t& slotId)
{
    std::string path = post::path + std::to_string(slotId);
    auto match_it = findPowerMatchByPath(path, postMatchVec);
    if (match_it == postMatchVec.end())
    {
        throw std::runtime_error("Post Match Not Created");
    }
    auto it = findPowerStateByPath(path, biosHasPost);
    if (it == biosHasPost.end())
    {
        return false;
    }

    return it->second;
}

bool isChassisOn(const size_t& slotId)
{
    std::string path = chassis::path + std::to_string(slotId);
    auto match_it = findPowerMatchByPath(path, chassisMatchVec);
    if (match_it == chassisMatchVec.end())
    {
        throw std::runtime_error("Chassis On Match Not Created");
    }
    auto it = findPowerStateByPath(path, chassisStatusOn);
    if (it == chassisStatusOn.end())
    {
        return false;
    }
    return it->second;
}

bool readingStateGood(const PowerState& powerState, const size_t& slotId)
{
    if (powerState == PowerState::on && !isPowerOn(slotId))
    {
        return false;
    }
    if (powerState == PowerState::biosPost &&
        (!hasBiosPost(slotId) || !isPowerOn(slotId)))
    {
        return false;
    }
    if (powerState == PowerState::chassisOn && !isChassisOn(slotId))
    {
        return false;
    }

    return true;
}

static void
    getPowerStatus(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                   size_t slotNumber = 0, size_t retries = 2)
{
    std::string busname = power::busname + std::to_string(slotNumber);
    std::string path = power::path + std::to_string(slotNumber);

    conn->async_method_call(
        [conn, retries, slotNumber,
         path](boost::system::error_code ec,
               const std::variant<std::string>& state) {
            if (ec)
            {
                if (retries != 0U)
                {
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        conn->get_io_context());
                    timer->expires_after(std::chrono::seconds(15));
                    timer->async_wait([timer, conn, retries,
                                       slotNumber](boost::system::error_code) {
                        getPowerStatus(conn, slotNumber, retries - 1);
                    });
                    return;
                }

                // we commonly come up before power control, we'll capture the
                // property change later
                std::cerr << "error getting power status " << ec.message()
                          << "\n";
                return;
            }
            auto it = findPowerStateByPath(path, powerStatusOn);
            it->second = std::get<std::string>(state).ends_with(".Running");
        },
        busname, path, properties::interface, properties::get, power::interface,
        power::property);
}

static void
    getPostStatus(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                  size_t slotNumber = 0, size_t retries = 2)
{
    std::string busname = post::busname + std::to_string(slotNumber);
    std::string path = post::path + std::to_string(slotNumber);

    conn->async_method_call(
        [conn, retries, slotNumber,
         path](boost::system::error_code ec,
               const std::variant<std::string>& state) {
            if (ec)
            {
                if (retries != 0U)
                {
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        conn->get_io_context());
                    timer->expires_after(std::chrono::seconds(15));
                    timer->async_wait([timer, conn, retries,
                                       slotNumber](boost::system::error_code) {
                        getPostStatus(conn, slotNumber, retries - 1);
                    });
                    return;
                }
                // we commonly come up before power control, we'll capture the
                // property change later
                std::cerr << "error getting post status " << ec.message()
                          << "\n";
                return;
            }
            const auto& value = std::get<std::string>(state);
            auto it = findPowerStateByPath(path, biosHasPost);
            it->second = (value != "Inactive") &&
                         (value != "xyz.openbmc_project.State.OperatingSystem."
                                   "Status.OSStatus.Inactive");
        },
        busname, path, properties::interface, properties::get, post::interface,
        post::property);
}

static void
    getChassisStatus(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                     size_t slotNumber = 0, size_t retries = 2)
{
    std::string busname = chassis::busname + std::to_string(slotNumber);
    std::string path = chassis::path + std::to_string(slotNumber);

    conn->async_method_call(
        [conn, retries, slotNumber,
         path](boost::system::error_code ec,
               const std::variant<std::string>& state) {
            if (ec)
            {
                if (retries != 0U)
                {
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        conn->get_io_context());
                    timer->expires_after(std::chrono::seconds(15));
                    timer->async_wait([timer, conn, retries,
                                       slotNumber](boost::system::error_code) {
                        getChassisStatus(conn, slotNumber, retries - 1);
                    });
                    return;
                }

                // we commonly come up before power control, we'll capture the
                // property change later
                std::cerr << "error getting chassis power status "
                          << ec.message() << "\n";
                return;
            }
            auto it = findPowerStateByPath(path, chassisStatusOn);
            it->second = std::get<std::string>(state).ends_with(chassis::sOn);
        },
        busname, path, properties::interface, properties::get,
        chassis::interface, chassis::property);
}

void setupPowerMatchCallback(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    std::function<void(PowerState type, bool state)>&& hostStatusCallback)
{
    // create a match for powergood changes, first time do a method call to
    // cache the correct value
    if (powerMatch)
    {
        return;
    }

    /* Get host paths */
    static const int depth = 1;
    GetSubTreePathsType hostSubTreePaths;

    try
    {
        auto method =
            conn->new_method_call(mapper::busName, mapper::path,
                                  mapper::interface, mapper::subtreepaths);
        method.append("/xyz/openbmc_project/state", depth,
                      GetSubTreePathsType({power::interface}));
        auto reply = conn->call(method);
        reply.read(hostSubTreePaths);
    }
    catch (sdbusplus::exception_t& e)
    {
        std::cerr << "Error getting host subtree paths: " << e.what() << "\n";
        return;
    }

    for (const auto& path : hostSubTreePaths)
    {
        size_t slotNumber =
            std::stoi(path.substr(path.find_last_of("/host") + 1));
        powerStatusOn.emplace_back(std::make_pair(std::string(path), false));
        biosHasPost.emplace_back(std::make_pair(std::string(path), false));
        auto timer =
            timerVec.emplace_back(std::make_shared<boost::asio::steady_timer>(
                conn->get_io_context()));

        powerMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*conn),
            "type='signal',interface='" + std::string(properties::interface) +
                "',path='" + std::string(path) + "',arg0='" +
                std::string(power::interface) + "'",
            [hostStatusCallback, path, timer](sdbusplus::message_t& message) {
                std::string objectName;
                boost::container::flat_map<std::string,
                                           std::variant<std::string>>
                    values;
                message.read(objectName, values);
                auto findState = values.find(power::property);
                if (findState != values.end())
                {
                    bool on = std::get<std::string>(findState->second)
                                  .ends_with(".Running");
                    auto it = findPowerStateByPath(path, powerStatusOn);
                    if (!on)
                    {
                        timer->cancel();
                        it->second = false;
                        hostStatusCallback(PowerState::on, it->second);
                        return;
                    }
                    // on comes too quickly
                    timer->expires_after(std::chrono::seconds(10));
                    timer->async_wait([hostStatusCallback,
                                       it](boost::system::error_code ec) {
                        if (ec == boost::asio::error::operation_aborted)
                        {
                            return;
                        }
                        if (ec)
                        {
                            std::cerr << "Timer error " << ec.message() << "\n";
                            return;
                        }
                        it->second = true;
                        hostStatusCallback(PowerState::on, it->second);
                    });
                }
            });

        powerMatchVec.emplace_back(
            std::make_pair(std::string(path), std::move(powerMatch)));

        postMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*conn),
            "type='signal',interface='" + std::string(properties::interface) +
                "',path='" + std::string(path) + "',arg0='" +
                std::string(post::interface) + "'",
            [hostStatusCallback, path](sdbusplus::message_t& message) {
                std::string objectName;
                boost::container::flat_map<std::string,
                                           std::variant<std::string>>
                    values;
                message.read(objectName, values);
                auto findState = values.find(post::property);
                if (findState != values.end())
                {
                    auto& value = std::get<std::string>(findState->second);
                    auto it = findPowerStateByPath(path, biosHasPost);
                    it->second =
                        (value != "Inactive") &&
                        (value != "xyz.openbmc_project.State.OperatingSystem."
                                  "Status.OSStatus.Inactive");
                    hostStatusCallback(PowerState::biosPost, it->second);
                }
            });
        postMatchVec.emplace_back(
            std::make_pair(std::string(path), std::move(postMatch)));
        getPowerStatus(conn, slotNumber);
        getPostStatus(conn, slotNumber);
    }

    /* Get chassis paths */
    GetSubTreePathsType chassisSubTreePaths;

    try
    {
        auto method =
            conn->new_method_call(mapper::busName, mapper::path,
                                  mapper::interface, mapper::subtreepaths);
        method.append("/xyz/openbmc_project/state", depth,
                      GetSubTreePathsType({chassis::interface}));
        auto reply = conn->call(method);
        reply.read(chassisSubTreePaths);
    }
    catch (sdbusplus::exception_t& e)
    {
        std::cerr << "Error getting chassis subtree paths: " << e.what()
                  << "\n";
        return;
    }

    for (const auto& path : chassisSubTreePaths)
    {
        chassisStatusOn.emplace_back(std::make_pair(std::string(path), false));
        size_t slotNumber =
            std::stoi(path.substr(path.find_last_of("/chassis") + 1));
        auto timerChassisOn = timerChassisStatusVec.emplace_back(
            std::make_shared<boost::asio::steady_timer>(
                conn->get_io_context()));

        chassisMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*conn),
            "type='signal',interface='" + std::string(properties::interface) +
                "',path='" + std::string(path) + "',arg0='" +
                std::string(chassis::interface) + "'",
            [hostStatusCallback, slotNumber, timerChassisOn,
             path](sdbusplus::message_t& message) {
                std::string objectName;
                boost::container::flat_map<std::string,
                                           std::variant<std::string>>
                    values;
                message.read(objectName, values);
                auto findState = values.find(chassis::property);
                if (findState != values.end())
                {
                    bool on = std::get<std::string>(findState->second)
                                  .ends_with(chassis::sOn);
                    auto it = findPowerStateByPath(path, chassisStatusOn);
                    if (!on)
                    {
                        timerChassisOn->cancel();
                        it->second = false;
                        hostStatusCallback(PowerState::chassisOn, it->second);
                        return;
                    }
                    // on comes too quickly
                    timerChassisOn->expires_after(std::chrono::seconds(10));
                    timerChassisOn->async_wait(
                        [hostStatusCallback, slotNumber,
                         it](boost::system::error_code ec) {
                            if (ec == boost::asio::error::operation_aborted)
                            {
                                return;
                            }
                            if (ec)
                            {
                                std::cerr
                                    << "Timer error " << ec.message() << "\n";
                                return;
                            }
                            it->second = true;
                            hostStatusCallback(PowerState::chassisOn,
                                               it->second);
                        });
                }
            });
        chassisMatchVec.emplace_back(
            std::make_pair(std::string(path), std::move(chassisMatch)));
        getChassisStatus(conn, slotNumber);
    }
}

void setupPowerMatch(const std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    setupPowerMatchCallback(conn, [](PowerState, bool) {});
}

// replaces limits if MinReading and MaxReading are found.
void findLimits(std::pair<double, double>& limits,
                const SensorBaseConfiguration* data)
{
    if (data == nullptr)
    {
        return;
    }
    auto maxFind = data->second.find("MaxReading");
    auto minFind = data->second.find("MinReading");

    if (minFind != data->second.end())
    {
        limits.first = std::visit(VariantToDoubleVisitor(), minFind->second);
    }
    if (maxFind != data->second.end())
    {
        limits.second = std::visit(VariantToDoubleVisitor(), maxFind->second);
    }
}

void createAssociation(
    std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path)
{
    if (association)
    {
        fs::path p(path);

        std::vector<Association> associations;
        associations.emplace_back("chassis", "all_sensors",
                                  p.parent_path().string());
        association->register_property("Associations", associations);
        association->initialize();
    }
}

void setInventoryAssociation(
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& inventoryPath, const std::string& chassisPath)
{
    if (association)
    {
        std::vector<Association> associations;
        associations.emplace_back("inventory", "sensors", inventoryPath);
        associations.emplace_back("chassis", "all_sensors", chassisPath);

        association->register_property("Associations", associations);
        association->initialize();
    }
}

std::optional<std::string> findContainingChassis(std::string_view configParent,
                                                 const GetSubTreeType& subtree)
{
    // A parent that is a chassis takes precedence
    for (const auto& [obj, services] : subtree)
    {
        if (obj == configParent)
        {
            return obj;
        }
    }

    // If the parent is not a chassis, the system chassis is used. This does not
    // work if there is more than one System, but we assume there is only one
    // today.
    for (const auto& [obj, services] : subtree)
    {
        for (const auto& [service, interfaces] : services)
        {
            if (std::find(interfaces.begin(), interfaces.end(),
                          "xyz.openbmc_project.Inventory.Item.System") !=
                interfaces.end())
            {
                return obj;
            }
        }
    }
    return std::nullopt;
}

void createInventoryAssoc(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path)
{
    if (!association)
    {
        return;
    }

    constexpr auto allInterfaces = std::to_array({
        "xyz.openbmc_project.Inventory.Item.Board",
        "xyz.openbmc_project.Inventory.Item.Chassis",
    });

    conn->async_method_call(
        [association, path](const boost::system::error_code ec,
                            const GetSubTreeType& subtree) {
            // The parent of the config is always the inventory object, and may
            // be the associated chassis. If the parent is not itself a chassis
            // or board, the sensor is associated with the system chassis.
            std::string parent = fs::path(path).parent_path().string();
            if (ec)
            {
                // In case of error, set the default associations and
                // initialize the association Interface.
                setInventoryAssociation(association, parent, parent);
                return;
            }
            setInventoryAssociation(
                association, parent,
                findContainingChassis(parent, subtree).value_or(parent));
        },
        mapper::busName, mapper::path, mapper::interface, "GetSubTree",
        "/xyz/openbmc_project/inventory/system", 2, allInterfaces);
}

std::optional<double> readFile(const std::string& thresholdFile,
                               const double& scaleFactor)
{
    std::string line;
    std::ifstream labelFile(thresholdFile);
    if (labelFile.good())
    {
        std::getline(labelFile, line);
        labelFile.close();

        try
        {
            return std::stod(line) / scaleFactor;
        }
        catch (const std::invalid_argument&)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::tuple<std::string, std::string, std::string>>
    splitFileName(const fs::path& filePath)
{
    if (filePath.has_filename())
    {
        const auto fileName = filePath.filename().string();

        size_t numberPos = std::strcspn(fileName.c_str(), "1234567890");
        size_t itemPos = std::strcspn(fileName.c_str(), "_");

        if (numberPos > 0 && itemPos > numberPos && fileName.size() > itemPos)
        {
            return std::make_optional(
                std::make_tuple(fileName.substr(0, numberPos),
                                fileName.substr(numberPos, itemPos - numberPos),
                                fileName.substr(itemPos + 1, fileName.size())));
        }
    }
    return std::nullopt;
}

static void handleSpecialModeChange(const std::string& manufacturingModeStatus)
{
    manufacturingMode = false;
    if (manufacturingModeStatus == "xyz.openbmc_project.Control.Security."
                                   "SpecialMode.Modes.Manufacturing")
    {
        manufacturingMode = true;
    }
    if (validateUnsecureFeature == 1)
    {
        if (manufacturingModeStatus == "xyz.openbmc_project.Control.Security."
                                       "SpecialMode.Modes.ValidationUnsecure")
        {
            manufacturingMode = true;
        }
    }
}

void setupManufacturingModeMatch(sdbusplus::asio::connection& conn)
{
    namespace rules = sdbusplus::bus::match::rules;
    static constexpr const char* specialModeInterface =
        "xyz.openbmc_project.Security.SpecialMode";

    const std::string filterSpecialModeIntfAdd =
        rules::interfacesAdded() +
        rules::argNpath(0, "/xyz/openbmc_project/security/special_mode");
    static std::unique_ptr<sdbusplus::bus::match_t> specialModeIntfMatch =
        std::make_unique<sdbusplus::bus::match_t>(
            conn, filterSpecialModeIntfAdd, [](sdbusplus::message_t& m) {
                sdbusplus::message::object_path path;
                using PropertyMap =
                    boost::container::flat_map<std::string,
                                               std::variant<std::string>>;
                boost::container::flat_map<std::string, PropertyMap>
                    interfaceAdded;
                m.read(path, interfaceAdded);
                auto intfItr = interfaceAdded.find(specialModeInterface);
                if (intfItr == interfaceAdded.end())
                {
                    return;
                }
                PropertyMap& propertyList = intfItr->second;
                auto itr = propertyList.find("SpecialMode");
                if (itr == propertyList.end())
                {
                    std::cerr << "error getting  SpecialMode property "
                              << "\n";
                    return;
                }
                auto* manufacturingModeStatus =
                    std::get_if<std::string>(&itr->second);
                handleSpecialModeChange(*manufacturingModeStatus);
            });

    const std::string filterSpecialModeChange =
        rules::type::signal() + rules::member("PropertiesChanged") +
        rules::interface("org.freedesktop.DBus.Properties") +
        rules::argN(0, specialModeInterface);
    static std::unique_ptr<sdbusplus::bus::match_t> specialModeChangeMatch =
        std::make_unique<sdbusplus::bus::match_t>(
            conn, filterSpecialModeChange, [](sdbusplus::message_t& m) {
                std::string interfaceName;
                boost::container::flat_map<std::string,
                                           std::variant<std::string>>
                    propertiesChanged;

                m.read(interfaceName, propertiesChanged);
                auto itr = propertiesChanged.find("SpecialMode");
                if (itr == propertiesChanged.end())
                {
                    return;
                }
                auto* manufacturingModeStatus =
                    std::get_if<std::string>(&itr->second);
                handleSpecialModeChange(*manufacturingModeStatus);
            });

    conn.async_method_call(
        [](const boost::system::error_code ec,
           const std::variant<std::string>& getManufactMode) {
            if (ec)
            {
                std::cerr << "error getting  SpecialMode status "
                          << ec.message() << "\n";
                return;
            }
            const auto* manufacturingModeStatus =
                std::get_if<std::string>(&getManufactMode);
            handleSpecialModeChange(*manufacturingModeStatus);
        },
        "xyz.openbmc_project.SpecialMode",
        "/xyz/openbmc_project/security/special_mode",
        "org.freedesktop.DBus.Properties", "Get", specialModeInterface,
        "SpecialMode");
}

bool getManufacturingMode()
{
    return manufacturingMode;
}

std::vector<std::unique_ptr<sdbusplus::bus::match_t>>
    setupPropertiesChangedMatches(
        sdbusplus::asio::connection& bus, std::span<const char* const> types,
        const std::function<void(sdbusplus::message_t&)>& handler)
{
    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches;
    for (const char* type : types)
    {
        auto match = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(bus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" +
                configInterfaceName(type) + "'",
            handler);
        matches.emplace_back(std::move(match));
    }
    return matches;
}

std::vector<std::unique_ptr<sdbusplus::bus::match_t>>
    setupPropertiesChangedMatches(
        sdbusplus::asio::connection& bus, const I2CDeviceTypeMap& typeMap,
        const std::function<void(sdbusplus::message_t&)>& handler)
{
    std::vector<const char*> types;
    types.reserve(typeMap.size());
    for (const auto& [type, dt] : typeMap)
    {
        types.push_back(type.data());
    }
    return setupPropertiesChangedMatches(bus, {types}, handler);
}
