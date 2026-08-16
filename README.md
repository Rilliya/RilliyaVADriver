# RilliyaVADriver

RilliyaVADriver is a clean-room macOS Audio Server Plug-in that publishes dynamically managed
virtual input and output devices. It uses only public Apple Core Audio and Core Foundation APIs.

The driver is source-only. It contains no certificate, provisioning profile, signing identity, or
pre-signed binary. A host application is responsible for assigning a unique product identity,
signing and notarizing the driver, and installing it with an appropriately signed installer.

## Build and test

Requirements:

- macOS 14.2 or later
- Xcode 26.3
- XcodeGen 2.46 or later

```sh
make check
```

The check covers formatting, deterministic product configuration, AddressSanitizer,
UndefinedBehaviorSanitizer, ThreadSanitizer, the public AudioServerPlugIn interface, Debug and
Release bundle builds, exported factory registration, and unexpected dynamic dependencies.

## Product identity

All downstream identity is declared in
`Configuration/ProductConfiguration.json`. Before distributing a derivative, assign unique values
for every identity in that file, then regenerate the checked-in header, xcconfig, and Info.plist:

```sh
./scripts/generate-product-configuration.sh
make check
```

Changing only the bundle identifier is unsafe. A derivative also needs its own factory UUID,
package identifier, model UID, device UID prefix, storage key, product name, and factory symbol.

Do not distribute a driver using Rilliya's default identity. Sign the `.driver` with your Developer
ID Application certificate, package it for `/Library/Audio/Plug-Ins/HAL`, sign the package with your
Developer ID Installer certificate, and notarize the final deliverable with your own Apple team.

## Integration boundary

The plug-in owns bounded realtime transport and HAL object publication. Endpoint configuration is
delivered through the `rlct` custom Core Audio property as a versioned property-list catalog.
RilliyaKit's `RilliyaVirtualAudio` product provides the corresponding application-side management
API without embedding this driver or any signing identity.

## License

Apache License 2.0. See `LICENSE`.
