/*
// Copyright (c) 2019 Intel Corporation
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

#include "PSUSensor.hpp"

#include "DeviceMgmt.hpp"
#include "SensorPaths.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"
#include "sensor.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/random_access_file.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static constexpr const char* sensorPathPrefix = "/xyz/openbmc_project/sensors/";

static constexpr bool debug = false;

PSUSensor::PSUSensor(
    const std::string& path, const std::string& objectType,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorName,
    std::vector<thresholds::Threshold>&& thresholdsIn,
    const std::string& sensorConfiguration, const PowerState& powerState,
    const std::string& sensorUnits, double factor, double max, double min,
    double offset, const std::string& label, size_t tSize, double pollRate,
    const std::shared_ptr<I2CDevice>& i2cDevice, const size_t& slotId) :
    Sensor(escapeName(sensorName), std::move(thresholdsIn), sensorConfiguration,
           objectType, false, false, max, min, conn, powerState, slotId),
    i2cDevice(i2cDevice), objServer(objectServer),
    inputDev(io, path, boost::asio::random_access_file::read_only),
    waitTimer(io), path(path), sensorFactor(factor), sensorOffset(offset),
    thresholdTimer(io), slotId(slotId)
{
    buffer = std::make_shared<std::array<char, 128>>();
    std::string unitPath = sensor_paths::getPathForUnits(sensorUnits);
    if constexpr (debug)
    {
        std::cerr << "Constructed sensor: path " << path << " type "
                  << objectType << " config " << sensorConfiguration
                  << " typename " << unitPath << " factor " << factor << " min "
                  << min << " max " << max << " offset " << offset << " name \""
                  << sensorName << "\"\n";
    }
    if (pollRate > 0.0)
    {
        sensorPollMs = static_cast<unsigned int>(pollRate * 1000);
    }

    std::string dbusPath = sensorPathPrefix + unitPath + "/" + name;

    sensorInterface = objectServer.add_interface(
        dbusPath, "xyz.openbmc_project.Sensor.Value");

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(dbusPath, interface);
    }

    // This should be called before initializing association.
    // createInventoryAssoc() does add more associations before doing
    // register and initialize "Associations" property.
    if (label.empty() || tSize == thresholds.size())
    {
        setInitialProperties(sensorUnits);
    }
    else
    {
        setInitialProperties(sensorUnits, label, tSize);
    }

    association = objectServer.add_interface(dbusPath, association::interface);

    createInventoryAssoc(conn, association, configurationPath);
}

PSUSensor::~PSUSensor()
{
    deactivate();

    objServer.remove_interface(sensorInterface);
    for (const auto& iface : thresholdInterfaces)
    {
        objServer.remove_interface(iface);
    }
    objServer.remove_interface(association);
}

bool PSUSensor::isActive()
{
    return inputDev.is_open();
}

void PSUSensor::activate(const std::string& newPath,
                         const std::shared_ptr<I2CDevice>& newI2CDevice)
{
    if (isActive())
    {
        // Avoid activating an active sensor
        return;
    }
    path = newPath;
    i2cDevice = newI2CDevice;
    inputDev.open(path, boost::asio::random_access_file::read_only);
    markAvailable(true);
    setupRead();
}

void PSUSensor::deactivate()
{
    markAvailable(false);
    // close the input dev to cancel async operations
    inputDev.close();
    waitTimer.cancel();
    i2cDevice = nullptr;
    path = "";
}

void PSUSensor::setupRead()
{
    if (!readingStateGood())
    {
        markAvailable(false);
        updateValue(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    if (buffer == nullptr)
    {
        std::cerr << "Buffer was invalid?";
        return;
    }

    std::weak_ptr<PSUSensor> weak = weak_from_this();
    // Note, we are building a asio buffer that is one char smaller than
    // the actual data structure, so that we can always append the null
    // terminator.  This can go away once std::from_chars<double> is available
    // in the standard
    inputDev.async_read_some_at(
        0, boost::asio::buffer(buffer->data(), buffer->size() - 1),
        [weak, buffer{buffer}](const boost::system::error_code& ec,
                               size_t bytesRead) {
            std::shared_ptr<PSUSensor> self = weak.lock();
            if (!self)
            {
                return;
            }

            self->handleResponse(ec, bytesRead);
        });
}

void PSUSensor::restartRead()
{
    std::weak_ptr<PSUSensor> weakRef = weak_from_this();
    waitTimer.expires_after(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "Failed to reschedule\n";
            return;
        }
        std::shared_ptr<PSUSensor> self = weakRef.lock();
        if (self)
        {
            self->setupRead();
        }
    });
}

// Create a buffer expected to be able to hold more characters than will be
// present in the input file.
void PSUSensor::handleResponse(const boost::system::error_code& err,
                               size_t bytesRead)
{
    if (err == boost::asio::error::operation_aborted)
    {
        std::cerr << "Read aborted\n";
        return;
    }
    if ((err == boost::system::errc::bad_file_descriptor) ||
        (err == boost::asio::error::misc_errors::not_found))
    {
        std::cerr << "Bad file descriptor for " << path << "\n";
        return;
    }
    if (err || bytesRead == 0)
    {
        if (readingStateGood())
        {
            std::cerr << name << " read failed\n";
        }
        restartRead();
        return;
    }

    // null terminate the string so we don't walk off the end
    std::array<char, 128>& bufferRef = *buffer;
    bufferRef[bytesRead] = '\0';

    try
    {
        rawValue = std::stod(bufferRef.data());
        updateValue((rawValue / sensorFactor) + sensorOffset);
    }
    catch (const std::invalid_argument&)
    {
        std::cerr << "Could not parse  input from " << path << "\n";
        incrementError();
    }

    restartRead();
}

void PSUSensor::checkThresholds()
{
    if (!readingStateGood())
    {
        return;
    }

    thresholds::checkThresholdsPowerDelay(weak_from_this(), thresholdTimer);
}
