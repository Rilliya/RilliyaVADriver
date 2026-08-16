#include "DriverCatalogCodec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace rilliya::audio_driver {
namespace {

const CFStringRef schemaVersionKey = CFSTR("schemaVersion");
const CFStringRef revisionKey = CFSTR("revision");
const CFStringRef endpointsKey = CFSTR("endpoints");
const CFStringRef identifierKey = CFSTR("id");
const CFStringRef nameKey = CFSTR("name");
const CFStringRef directionKey = CFSTR("direction");
const CFStringRef sampleRateKey = CFSTR("sampleRate");
const CFStringRef channelCountKey = CFSTR("channelCount");
const CFStringRef inputDirection = CFSTR("input");
const CFStringRef outputDirection = CFSTR("output");

template <typename Reference> class CFReference final {
public:
  explicit CFReference(Reference value = nullptr) noexcept : value_(value) {}
  ~CFReference() {
    if (value_ != nullptr) {
      CFRelease(value_);
    }
  }

  CFReference(const CFReference&) = delete;
  CFReference& operator=(const CFReference&) = delete;

  CFReference(CFReference&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  CFReference& operator=(CFReference&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (value_ != nullptr) {
      CFRelease(value_);
    }
    value_ = std::exchange(other.value_, nullptr);
    return *this;
  }

  [[nodiscard]] Reference get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
  Reference value_;
};

[[nodiscard]] bool hasType(CFTypeRef value, CFTypeID expectedType) noexcept {
  return value != nullptr && CFGetTypeID(value) == expectedType;
}

[[nodiscard]] CFTypeRef dictionaryValue(CFDictionaryRef dictionary, CFStringRef key) noexcept {
  return static_cast<CFTypeRef>(CFDictionaryGetValue(dictionary, key));
}

[[nodiscard]] bool readUInt64(CFTypeRef value, std::uint64_t& result) noexcept {
  if (!hasType(value, CFNumberGetTypeID())) {
    return false;
  }
  std::int64_t signedValue = 0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &signedValue) ||
      signedValue < 0) {
    return false;
  }
  result = static_cast<std::uint64_t>(signedValue);
  return true;
}

[[nodiscard]] bool readDouble(CFTypeRef value, double& result) noexcept {
  if (!hasType(value, CFNumberGetTypeID())) {
    return false;
  }
  return CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &result);
}

[[nodiscard]] CFReference<CFMutableStringRef> normalizedName(CFStringRef name) {
  CFReference<CFMutableStringRef> normalized(
      CFStringCreateMutableCopy(kCFAllocatorDefault, 0, name));
  if (!normalized) {
    return CFReference<CFMutableStringRef>();
  }
  CFStringTrimWhitespace(normalized.get());
  CFStringNormalize(normalized.get(), kCFStringNormalizationFormC);
  return normalized;
}

[[nodiscard]] bool containsControlCharacter(CFStringRef string) noexcept {
  const CFCharacterSetRef controls = CFCharacterSetGetPredefined(kCFCharacterSetControl);
  const CFIndex length = CFStringGetLength(string);
  for (CFIndex index = 0; index < length; ++index) {
    if (CFCharacterSetIsCharacterMember(controls, CFStringGetCharacterAtIndex(string, index))) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool copyUTF8Name(CFStringRef string, std::string& result) {
  std::array<char, EndpointRegistry::maximumNameByteCount + 1> buffer{};
  if (!CFStringGetCString(string, buffer.data(), static_cast<CFIndex>(buffer.size()),
                          kCFStringEncodingUTF8)) {
    return false;
  }
  result = buffer.data();
  return !result.empty() && result.size() <= EndpointRegistry::maximumNameByteCount;
}

[[nodiscard]] bool parseIdentifier(CFStringRef string, EndpointIdentifier& identifier) {
  CFReference<CFUUIDRef> uuid(CFUUIDCreateFromString(kCFAllocatorDefault, string));
  if (!uuid) {
    return false;
  }
  const CFUUIDBytes bytes = CFUUIDGetUUIDBytes(uuid.get());
  identifier = {bytes.byte0,  bytes.byte1,  bytes.byte2,  bytes.byte3, bytes.byte4,  bytes.byte5,
                bytes.byte6,  bytes.byte7,  bytes.byte8,  bytes.byte9, bytes.byte10, bytes.byte11,
                bytes.byte12, bytes.byte13, bytes.byte14, bytes.byte15};
  return std::any_of(identifier.begin(), identifier.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

[[nodiscard]] CFStringRef createString(std::string_view string) {
  return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(string.data()),
                                 static_cast<CFIndex>(string.size()), kCFStringEncodingUTF8, false);
}

void appendDictionaryValue(CFMutableDictionaryRef dictionary, CFStringRef key, CFTypeRef value) {
  if (value != nullptr) {
    CFDictionarySetValue(dictionary, key, value);
  }
}

[[nodiscard]] CFNumberRef createUInt64Number(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return nullptr;
  }
  const std::int64_t signedValue = static_cast<std::int64_t>(value);
  return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &signedValue);
}

} // namespace

DriverCatalogDecodeResult decodeDriverCatalog(CFPropertyListRef propertyList) {
  if (!hasType(propertyList, CFDictionaryGetTypeID())) {
    return {.error = DriverCatalogCodecError::rootIsNotDictionary};
  }
  const auto dictionary = static_cast<CFDictionaryRef>(propertyList);

  const CFTypeRef schemaValue = dictionaryValue(dictionary, schemaVersionKey);
  if (schemaValue == nullptr) {
    return {.error = DriverCatalogCodecError::missingSchemaVersion};
  }
  std::uint64_t schemaVersion = 0;
  if (!readUInt64(schemaValue, schemaVersion)) {
    return {.error = DriverCatalogCodecError::missingSchemaVersion};
  }
  if (schemaVersion != driverCatalogSchemaVersion) {
    return {.error = DriverCatalogCodecError::unsupportedSchemaVersion};
  }

  const CFTypeRef revisionValue = dictionaryValue(dictionary, revisionKey);
  if (revisionValue == nullptr) {
    return {.error = DriverCatalogCodecError::missingRevision};
  }
  std::uint64_t revision = 0;
  if (!readUInt64(revisionValue, revision)) {
    return {.error = DriverCatalogCodecError::invalidRevision};
  }

  const CFTypeRef endpointsValue = dictionaryValue(dictionary, endpointsKey);
  if (endpointsValue == nullptr) {
    return {.error = DriverCatalogCodecError::missingEndpoints};
  }
  if (!hasType(endpointsValue, CFArrayGetTypeID())) {
    return {.error = DriverCatalogCodecError::endpointsIsNotArray};
  }
  const auto endpoints = static_cast<CFArrayRef>(endpointsValue);
  const CFIndex endpointCount = CFArrayGetCount(endpoints);
  if (endpointCount < 0 ||
      static_cast<std::size_t>(endpointCount) > EndpointRegistry::maximumEndpointCount) {
    return {.error = DriverCatalogCodecError::tooManyEndpoints};
  }

  DriverEndpointCatalog decoded{.revision = revision};
  decoded.endpoints.reserve(static_cast<std::size_t>(endpointCount));
  std::vector<CFReference<CFStringRef>> normalizedNames;
  normalizedNames.reserve(static_cast<std::size_t>(endpointCount));

  for (CFIndex rawIndex = 0; rawIndex < endpointCount; ++rawIndex) {
    const std::size_t index = static_cast<std::size_t>(rawIndex);
    const CFTypeRef endpointValue =
        static_cast<CFTypeRef>(CFArrayGetValueAtIndex(endpoints, rawIndex));
    if (!hasType(endpointValue, CFDictionaryGetTypeID())) {
      return {.error = DriverCatalogCodecError::endpointIsNotDictionary, .endpointIndex = index};
    }
    const auto endpointDictionary = static_cast<CFDictionaryRef>(endpointValue);

    const CFTypeRef identifierValue = dictionaryValue(endpointDictionary, identifierKey);
    if (identifierValue == nullptr) {
      return {.error = DriverCatalogCodecError::missingIdentifier, .endpointIndex = index};
    }
    EndpointIdentifier identifier{};
    if (!hasType(identifierValue, CFStringGetTypeID()) ||
        !parseIdentifier(static_cast<CFStringRef>(identifierValue), identifier)) {
      return {.error = DriverCatalogCodecError::invalidIdentifier, .endpointIndex = index};
    }
    if (std::any_of(
            decoded.endpoints.begin(), decoded.endpoints.end(),
            [&](const EndpointDefinition& prior) { return prior.identifier == identifier; })) {
      return {.error = DriverCatalogCodecError::duplicateIdentifier, .endpointIndex = index};
    }

    const CFTypeRef nameValue = dictionaryValue(endpointDictionary, nameKey);
    if (nameValue == nullptr) {
      return {.error = DriverCatalogCodecError::missingName, .endpointIndex = index};
    }
    if (!hasType(nameValue, CFStringGetTypeID())) {
      return {.error = DriverCatalogCodecError::invalidName, .endpointIndex = index};
    }
    CFReference<CFMutableStringRef> normalized =
        normalizedName(static_cast<CFStringRef>(nameValue));
    std::string name;
    if (!normalized || CFStringGetLength(normalized.get()) == 0 ||
        containsControlCharacter(normalized.get()) || !copyUTF8Name(normalized.get(), name)) {
      return {.error = DriverCatalogCodecError::invalidName, .endpointIndex = index};
    }
    for (const auto& priorName : normalizedNames) {
      if (CFStringCompare(priorName.get(), normalized.get(),
                          kCFCompareCaseInsensitive | kCFCompareNonliteral) == kCFCompareEqualTo) {
        return {.error = DriverCatalogCodecError::duplicateName, .endpointIndex = index};
      }
    }
    normalizedNames.emplace_back(static_cast<CFStringRef>(CFRetain(normalized.get())));

    const CFTypeRef directionValue = dictionaryValue(endpointDictionary, directionKey);
    if (directionValue == nullptr) {
      return {.error = DriverCatalogCodecError::missingDirection, .endpointIndex = index};
    }
    if (!hasType(directionValue, CFStringGetTypeID())) {
      return {.error = DriverCatalogCodecError::invalidDirection, .endpointIndex = index};
    }
    const auto directionString = static_cast<CFStringRef>(directionValue);
    EndpointDirection direction;
    if (CFEqual(directionString, inputDirection)) {
      direction = EndpointDirection::input;
    } else if (CFEqual(directionString, outputDirection)) {
      direction = EndpointDirection::output;
    } else {
      return {.error = DriverCatalogCodecError::invalidDirection, .endpointIndex = index};
    }

    const CFTypeRef sampleRateValue = dictionaryValue(endpointDictionary, sampleRateKey);
    if (sampleRateValue == nullptr) {
      return {.error = DriverCatalogCodecError::missingSampleRate, .endpointIndex = index};
    }
    double sampleRate = 0;
    if (!readDouble(sampleRateValue, sampleRate) || !std::isfinite(sampleRate) || sampleRate < 1 ||
        sampleRate > 768000 || std::trunc(sampleRate) != sampleRate) {
      return {.error = DriverCatalogCodecError::invalidSampleRate, .endpointIndex = index};
    }

    const CFTypeRef channelCountValue = dictionaryValue(endpointDictionary, channelCountKey);
    if (channelCountValue == nullptr) {
      return {.error = DriverCatalogCodecError::missingChannelCount, .endpointIndex = index};
    }
    std::uint64_t channelCount = 0;
    if (!readUInt64(channelCountValue, channelCount) || channelCount == 0 || channelCount > 256) {
      return {.error = DriverCatalogCodecError::invalidChannelCount, .endpointIndex = index};
    }

    decoded.endpoints.push_back(EndpointDefinition{
        .identifier = identifier,
        .name = std::move(name),
        .direction = direction,
        .sampleRate = sampleRate,
        .channelCount = static_cast<std::uint32_t>(channelCount),
    });
  }

  return {.catalog = std::move(decoded)};
}

CFDictionaryRef createDriverCatalogPropertyList(const DriverEndpointCatalog& catalog) {
  CFReference<CFMutableDictionaryRef> root(CFDictionaryCreateMutable(
      kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CFReference<CFNumberRef> schema(createUInt64Number(driverCatalogSchemaVersion));
  CFReference<CFNumberRef> revision(createUInt64Number(catalog.revision));
  CFReference<CFMutableArrayRef> endpoints(CFArrayCreateMutable(
      kCFAllocatorDefault, static_cast<CFIndex>(catalog.endpoints.size()), &kCFTypeArrayCallBacks));
  if (!root || !schema || !revision || !endpoints) {
    return nullptr;
  }

  appendDictionaryValue(root.get(), schemaVersionKey, schema.get());
  appendDictionaryValue(root.get(), revisionKey, revision.get());
  for (const EndpointDefinition& endpoint : catalog.endpoints) {
    CFReference<CFMutableDictionaryRef> dictionary(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFReference<CFStringRef> identifier(
        createString(EndpointRegistry::identifierString(endpoint.identifier)));
    CFReference<CFStringRef> name(createString(endpoint.name));
    CFReference<CFNumberRef> sampleRate(
        CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &endpoint.sampleRate));
    const std::int64_t channelCount = endpoint.channelCount;
    CFReference<CFNumberRef> channels(
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &channelCount));
    if (!dictionary || !identifier || !name || !sampleRate || !channels) {
      return nullptr;
    }
    appendDictionaryValue(dictionary.get(), identifierKey, identifier.get());
    appendDictionaryValue(dictionary.get(), nameKey, name.get());
    appendDictionaryValue(dictionary.get(), directionKey,
                          endpoint.direction == EndpointDirection::input ? inputDirection
                                                                         : outputDirection);
    appendDictionaryValue(dictionary.get(), sampleRateKey, sampleRate.get());
    appendDictionaryValue(dictionary.get(), channelCountKey, channels.get());
    CFArrayAppendValue(endpoints.get(), dictionary.get());
  }
  appendDictionaryValue(root.get(), endpointsKey, endpoints.get());
  return static_cast<CFDictionaryRef>(CFRetain(root.get()));
}

} // namespace rilliya::audio_driver
