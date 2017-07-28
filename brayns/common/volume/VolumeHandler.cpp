/* Copyright (c) 2015-2017, EPFL/Blue Brain Project
 * All rights reserved. Do not distribute without permission.
 * Responsible Author: Cyrille Favreau <cyrille.favreau@epfl.ch>
 *
 * This file is part of Brayns <https://github.com/BlueBrain/Brayns>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3.0 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "VolumeHandler.h"

#include <brayns/common/log.h>

#include <fcntl.h>
#include <fstream>
#include <future>
#include <sys/mman.h>
#include <sys/stat.h>

namespace
{
const int NO_DESCRIPTOR = -1;
}

namespace brayns
{
VolumeHandler::VolumeHandler(const VolumeParameters& volumeParameters,
                             const TimestampMode timestampMode)
    : _volumeParameters(volumeParameters)
    , _timestamp(std::numeric_limits<float>::max())
    , _timestampRange(std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::min())
    , _timestampMode(timestampMode)
{
}

VolumeHandler::~VolumeHandler()
{
    _volumeDescriptors.clear();
}

void VolumeHandler::attachVolumeToFile(const float timestamp,
                                       const std::string& volumeFile)
{
    // Add volume descriptor for specified timestamp
    _volumeDescriptors[timestamp].reset(
        new VolumeDescriptor(volumeFile, _volumeParameters.getDimensions(),
                             _volumeParameters.getElementSpacing(),
                             _volumeParameters.getOffset()));

    // Update timestamp range
    for (const auto& volumeDescriptor : _volumeDescriptors)
    {
        _timestampRange.x() =
            std::min(_timestampRange.x(), volumeDescriptor.first);
        _timestampRange.y() =
            std::max(_timestampRange.y(), volumeDescriptor.first);
    }
    BRAYNS_INFO << "Attached " << volumeFile << " to timestamp " << timestamp
                << " " << _timestampRange << std::endl;
}

void VolumeHandler::setTimestamp(const float timestamp)
{
    const float ts = _getBoundedTimestamp(timestamp);
    if (ts != _timestamp &&
        _volumeDescriptors.find(ts) != _volumeDescriptors.end())
    {
        if (_volumeDescriptors.find(_timestamp) != _volumeDescriptors.end())
            _volumeDescriptors[_timestamp]->unmap();
        _timestamp = ts;
        _volumeDescriptors[_timestamp]->map();
    }
}

void* VolumeHandler::getData() const
{
    if (_volumeDescriptors.find(_timestamp) != _volumeDescriptors.end())
        return _volumeDescriptors.at(_timestamp)->getMemoryMapPtr();
    return nullptr;
}

float VolumeHandler::getEpsilon(const Vector3f& elementSpacing,
                                const uint16_t samplesPerRay)
{
    if (_volumeDescriptors.find(_timestamp) == _volumeDescriptors.end())
        return 0.f;
    const Vector3f diag =
        elementSpacing * _volumeDescriptors.at(_timestamp)->getDimensions();
    const float diagMax = diag.find_max();
    const float epsilon = diagMax / float(samplesPerRay);
    return std::max(1.f, epsilon);
}

Vector3ui VolumeHandler::getDimensions() const
{
    if (_volumeDescriptors.find(_timestamp) != _volumeDescriptors.end())
        return _volumeDescriptors.at(_timestamp)->getDimensions();
    return Vector3ui();
}

Vector3f VolumeHandler::getElementSpacing() const
{
    if (_volumeDescriptors.find(_timestamp) != _volumeDescriptors.end())
        return _volumeDescriptors.at(_timestamp)->getElementSpacing();
    return Vector3f();
}

Vector3f VolumeHandler::getOffset() const
{
    if (_volumeDescriptors.find(_timestamp) != _volumeDescriptors.end())
        return _volumeDescriptors.at(_timestamp)->getOffset();
    return Vector3f();
}

uint64_t VolumeHandler::getSize() const
{
    if (_volumeDescriptors.find(_timestamp) != _volumeDescriptors.end())
        return _volumeDescriptors.at(_timestamp)->getSize();
    return 0;
}

float VolumeHandler::_getBoundedTimestamp(const float timestamp) const
{
    float result = 0.f;
    switch (_timestampMode)
    {
    case TimestampMode::modulo:
        if (_volumeDescriptors.size() != 0)
            result = size_t(timestamp + _timestampRange.x()) %
                     _volumeDescriptors.size();
        break;
    case TimestampMode::bounded:
        result = std::max(std::min(timestamp, _timestampRange.y()),
                          _timestampRange.x());
    case TimestampMode::unchanged:
    default:
        result = timestamp;
    }
    return result;
}

VolumeHandler::VolumeDescriptor::VolumeDescriptor(
    const std::string& filename, const Vector3ui& dimensions,
    const Vector3f& elementSpacing, const Vector3f& offset)
    : _filename(filename)
    , _memoryMapPtr(0)
    , _cacheFileDescriptor(NO_DESCRIPTOR)
    , _dimensions(dimensions)
    , _elementSpacing(elementSpacing)
    , _offset(offset)
{
}

VolumeHandler::VolumeDescriptor::~VolumeDescriptor()
{
    unmap();
}

void VolumeHandler::VolumeDescriptor::map()
{
    _cacheFileDescriptor = open(_filename.c_str(), O_RDONLY);
    if (_cacheFileDescriptor == NO_DESCRIPTOR)
    {
        BRAYNS_ERROR << "Failed to attach " << _filename << std::endl;
        return;
    }

    struct stat sb;
    if (::fstat(_cacheFileDescriptor, &sb) == NO_DESCRIPTOR)
    {
        BRAYNS_ERROR << "Failed to attach " << _filename << std::endl;
        return;
    }

    _size = sb.st_size;
    _memoryMapPtr =
        ::mmap(0, _size, PROT_READ, MAP_PRIVATE, _cacheFileDescriptor, 0);
    if (_memoryMapPtr == MAP_FAILED)
    {
        _memoryMapPtr = 0;
        ::close(_cacheFileDescriptor);
        _cacheFileDescriptor = NO_DESCRIPTOR;
        BRAYNS_ERROR << "Failed to attach " << _filename << std::endl;
        return;
    }
}

void VolumeHandler::VolumeDescriptor::unmap()
{
    if (_memoryMapPtr)
    {
        ::munmap((void*)_memoryMapPtr, _size);
        _memoryMapPtr = 0;
    }
    if (_cacheFileDescriptor != NO_DESCRIPTOR)
    {
        ::close(_cacheFileDescriptor);
        _cacheFileDescriptor = NO_DESCRIPTOR;
    }
}

const Histogram& VolumeHandler::getHistogram()
{
    if (_histograms.find(_timestamp) != _histograms.end())
        return _histograms[_timestamp];

    std::future<bool> computeHistogram =
        std::async(std::launch::async, [this]() {
            uint8_t* data = static_cast<uint8_t*>(getData());
            if (data)
            {
                BRAYNS_INFO << "Computing volume histogram" << std::endl;
                uint8_t minValue = std::numeric_limits<uint8_t>::max();
                uint8_t maxValue = 0;
                std::map<uint8_t, uint64_t> values;
                for (uint64_t i = 0; i < getSize(); ++i)
                {
                    const uint8_t value = data[i];
                    minValue = std::min(minValue, value);
                    maxValue = std::max(maxValue, value);
                    if (values.find(value) == values.end())
                        values[value] = 1;
                    else
                        ++values[value];
                }

                _histograms[_timestamp].values.clear();
                for (const auto& value : values)
                    _histograms[_timestamp].values.push_back(value.second);
                _histograms[_timestamp].range = Vector2f(minValue, maxValue);
                BRAYNS_INFO
                    << "Histogram range: " << _histograms[_timestamp].range
                    << std::endl;
            }
            return true;
        });

    computeHistogram.wait();
    computeHistogram.get();

    return _histograms[_timestamp];
}
}
