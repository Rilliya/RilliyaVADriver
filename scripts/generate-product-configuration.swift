import Foundation

private struct ProductConfiguration: Decodable {
  let schemaVersion: Int
  let productName: String
  let bundleIdentifier: String
  let packageIdentifier: String
  let factoryUUID: String
  let factorySymbol: String
  let manufacturerName: String
  let plugInName: String
  let modelUID: String
  let deviceUIDPrefix: String
  let catalogStorageKey: String
  let internalDeviceNamePrefix: String
}

private enum GenerationError: Error, CustomStringConvertible {
  case usage
  case invalid(String)

  var description: String {
    switch self {
    case .usage:
      return
        "Usage: generate-product-configuration.swift <configuration.json> <header> <xcconfig> <Info.plist>"
    case .invalid(let message):
      return "Invalid product configuration: \(message)"
    }
  }
}

private let audioServerPlugInTypeUUID = "443ABAB8-E7B3-491A-B985-BEB9187030DB"

private func isPrintableASCII(_ value: String) -> Bool {
  !value.isEmpty && value.utf8.allSatisfy { $0 >= 0x20 && $0 <= 0x7E }
}

private func isIdentifier(_ value: String) -> Bool {
  guard let first = value.utf8.first,
    (first >= 0x41 && first <= 0x5A) || (first >= 0x61 && first <= 0x7A) || first == 0x5F
  else {
    return false
  }
  return value.utf8.dropFirst().allSatisfy {
    ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x41 && $0 <= 0x5A)
      || ($0 >= 0x61 && $0 <= 0x7A) || $0 == 0x5F
  }
}

private func isReverseDNSIdentifier(_ value: String) -> Bool {
  let components = value.split(separator: ".", omittingEmptySubsequences: false)
  guard components.count >= 3 else { return false }
  return components.allSatisfy { component in
    guard !component.isEmpty else { return false }
    return component.utf8.allSatisfy {
      ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x41 && $0 <= 0x5A)
        || ($0 >= 0x61 && $0 <= 0x7A) || $0 == 0x2D
    }
  }
}

private func validate(_ configuration: ProductConfiguration) throws {
  guard configuration.schemaVersion == 1 else {
    throw GenerationError.invalid("schemaVersion must be 1")
  }

  let printableValues = [
    configuration.productName,
    configuration.manufacturerName,
    configuration.plugInName,
    configuration.catalogStorageKey,
    configuration.internalDeviceNamePrefix,
  ]
  guard printableValues.allSatisfy({ isPrintableASCII($0) && $0.utf8.count <= 128 }) else {
    throw GenerationError.invalid(
      "display and storage values must be 1...128 printable ASCII bytes")
  }
  guard isIdentifier(configuration.productName) else {
    throw GenerationError.invalid("productName must be a C-style identifier")
  }
  guard isIdentifier(configuration.factorySymbol) else {
    throw GenerationError.invalid("factorySymbol must be a C-style identifier")
  }
  guard isReverseDNSIdentifier(configuration.bundleIdentifier) else {
    throw GenerationError.invalid("bundleIdentifier must use reverse-DNS components")
  }
  guard isReverseDNSIdentifier(configuration.packageIdentifier) else {
    throw GenerationError.invalid("packageIdentifier must use reverse-DNS components")
  }
  guard isReverseDNSIdentifier(configuration.modelUID) else {
    throw GenerationError.invalid("modelUID must use reverse-DNS components")
  }
  guard configuration.deviceUIDPrefix.hasSuffix("."),
    isReverseDNSIdentifier(String(configuration.deviceUIDPrefix.dropLast()))
  else {
    throw GenerationError.invalid("deviceUIDPrefix must be a reverse-DNS prefix ending in a period")
  }
  guard let factoryUUID = UUID(uuidString: configuration.factoryUUID),
    factoryUUID != UUID(uuid: (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
  else {
    throw GenerationError.invalid("factoryUUID must be a nonzero UUID")
  }
  guard configuration.bundleIdentifier != configuration.packageIdentifier,
    configuration.bundleIdentifier != configuration.modelUID
  else {
    throw GenerationError.invalid("bundle, package, and model identities must be distinct")
  }
}

private func cLiteral(_ value: String) -> String {
  let escaped =
    value
    .replacingOccurrences(of: "\\", with: "\\\\")
    .replacingOccurrences(of: "\"", with: "\\\"")
  return "\"\(escaped)\""
}

private func write(_ string: String, to url: URL) throws {
  try FileManager.default.createDirectory(
    at: url.deletingLastPathComponent(),
    withIntermediateDirectories: true
  )
  try Data(string.utf8).write(to: url, options: .atomic)
}

private func generate() throws {
  guard CommandLine.arguments.count == 5 else { throw GenerationError.usage }
  let configurationURL = URL(fileURLWithPath: CommandLine.arguments[1])
  let headerURL = URL(fileURLWithPath: CommandLine.arguments[2])
  let xcconfigURL = URL(fileURLWithPath: CommandLine.arguments[3])
  let infoPlistURL = URL(fileURLWithPath: CommandLine.arguments[4])

  let configuration = try JSONDecoder().decode(
    ProductConfiguration.self,
    from: Data(contentsOf: configurationURL)
  )
  try validate(configuration)

  let header = """
    // Generated from Configuration/ProductConfiguration.json. Do not edit directly.
    #pragma once

    #include <CoreFoundation/CoreFoundation.h>

    #include <string_view>

    #define RILLIYA_VA_DRIVER_FACTORY_SYMBOL \(configuration.factorySymbol)

    namespace rilliya::audio_driver::product_configuration {

    inline constexpr std::string_view productName = \(cLiteral(configuration.productName));
    inline constexpr std::string_view bundleIdentifier = \(cLiteral(configuration.bundleIdentifier));
    inline constexpr std::string_view packageIdentifier = \(cLiteral(configuration.packageIdentifier));
    inline constexpr std::string_view factoryUUID = \(cLiteral(configuration.factoryUUID.uppercased()));
    inline constexpr std::string_view factorySymbol = \(cLiteral(configuration.factorySymbol));
    inline constexpr std::string_view deviceUIDPrefix = \(cLiteral(configuration.deviceUIDPrefix));
    inline const CFStringRef catalogStorageKey = CFSTR(\(cLiteral(configuration.catalogStorageKey)));
    inline const CFStringRef manufacturerName = CFSTR(\(cLiteral(configuration.manufacturerName)));
    inline const CFStringRef plugInName = CFSTR(\(cLiteral(configuration.plugInName)));
    inline const CFStringRef modelUID = CFSTR(\(cLiteral(configuration.modelUID)));
    inline constexpr std::string_view internalDeviceNamePrefix = \(cLiteral(configuration.internalDeviceNamePrefix));

    } // namespace rilliya::audio_driver::product_configuration

    """
  try write(header, to: headerURL)

  let xcconfig = """
    // Generated from Configuration/ProductConfiguration.json. Do not edit directly.
    PRODUCT_BUNDLE_IDENTIFIER = \(configuration.bundleIdentifier)
    PRODUCT_NAME = \(configuration.productName)
    RILLIYA_VA_DRIVER_PACKAGE_IDENTIFIER = \(configuration.packageIdentifier)
    """
  try write(xcconfig + "\n", to: xcconfigURL)

  let infoPlist: [String: Any] = [
    "CFBundleDevelopmentRegion": "$(DEVELOPMENT_LANGUAGE)",
    "CFBundleExecutable": "$(EXECUTABLE_NAME)",
    "CFBundleIdentifier": "$(PRODUCT_BUNDLE_IDENTIFIER)",
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundleName": "$(PRODUCT_NAME)",
    "CFBundlePackageType": "BNDL",
    "CFBundleShortVersionString": "$(MARKETING_VERSION)",
    "CFBundleVersion": "$(CURRENT_PROJECT_VERSION)",
    "CFPlugInFactories": [configuration.factoryUUID.uppercased(): configuration.factorySymbol],
    "CFPlugInTypes": [audioServerPlugInTypeUUID: [configuration.factoryUUID.uppercased()]],
  ]
  let plistData = try PropertyListSerialization.data(
    fromPropertyList: infoPlist,
    format: .xml,
    options: 0
  )
  try plistData.write(to: infoPlistURL, options: .atomic)
}

do {
  try generate()
} catch {
  FileHandle.standardError.write(Data("\(error)\n".utf8))
  exit(1)
}
