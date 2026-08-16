#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

class TestFailure final : public std::runtime_error {
public:
  explicit TestFailure(const char* message) : std::runtime_error(message) {}
};

void expect(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

} // namespace

int main(int argumentCount, const char* arguments[]) {
  try {
    expect(argumentCount == 2, "expected a path to the built driver bundle");
    const CFURLRef bundleURL = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(arguments[1]),
        static_cast<CFIndex>(std::char_traits<char>::length(arguments[1])), true);
    expect(bundleURL != nullptr, "driver bundle URL should be created");

    const CFPlugInRef plugIn = CFPlugInCreate(kCFAllocatorDefault, bundleURL);
    CFRelease(bundleURL);
    expect(plugIn != nullptr, "driver bundle should load as a CFPlugIn");

    const CFArrayRef factories =
        CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plugIn);
    expect(factories != nullptr && CFArrayGetCount(factories) == 1,
           "driver should register exactly one Audio Server Plug-in factory");
    const CFUUIDRef factoryIdentifier =
        static_cast<CFUUIDRef>(CFArrayGetValueAtIndex(factories, 0));
    void* instance =
        CFPlugInInstanceCreate(kCFAllocatorDefault, factoryIdentifier, kAudioServerPlugInTypeUUID);
    expect(instance != nullptr, "registered factory should create a driver instance");

    auto driver = static_cast<AudioServerPlugInDriverRef>(instance);
    void* queried = nullptr;
    const HRESULT result = (*driver)->QueryInterface(
        driver, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &queried);
    expect(result == S_OK && queried == driver,
           "factory instance should implement the public driver interface");
    (*driver)->Release(driver);

    CFRelease(factories);
    CFRelease(plugIn);
    std::cout << "Driver bundle registration and factory test passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "Driver bundle test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
