# Error handling

Mais normalizes every backend error (Vulkan, OpenXR, GLFW) into a single
taxonomy, `mais::Error`, defined in `headers/mais/Error.hpp`. Backends map their
native result codes at the boundary so consumers react uniformly.

## The taxonomy

| `mais::Error`      | Meaning                                            | Handling          |
| ------------------ | -------------------------------------------------- | ----------------- |
| `Ok`               | Success                                            | Continue          |
| `Suboptimal`       | `VK_SUBOPTIMAL_KHR` (presentable, recreate soon)   | Handle            |
| `SwapchainOutOfDate` | `VK_ERROR_OUT_OF_DATE_KHR` / `VK_ERROR_SURFACE_LOST_KHR` | Handle (engine already recreates) |
| `NotReady`         | `VK_NOT_READY` / XR session not running / invalid time | Retry or skip frame |
| `DeviceLost`       | `VK_ERROR_DEVICE_LOST`                             | Fatal             |
| `RuntimeLost`      | XR session/instance loss, `XR_SESSION_STATE_LOSS_PENDING` | Fatal     |
| `OutOfMemory`      | `VK_ERROR_OUT_OF_{HOST,DEVICE}_MEMORY`             | Fatal             |
| `RuntimeError`     | Anything else (including GLFW errors)              | Fatal             |

Helpers: `isOk`, `isFatal`, `isRecoverable`.

## Consumer contract

Frame operations return `mais::Error`:

- `Engine::update()` → `Error`
- `Engine::render()` → `Error`
- `Renderer::drawFrame()` → `Error`
- `ADeviceBackend::preprocessFrame/processFrame/postprocessFrame` → `Error`

Handle recoverable errors (`Suboptimal`, `SwapchainOutOfDate`, `NotReady`) and
keep running. On a fatal error (`DeviceLost`, `RuntimeLost`, `OutOfMemory`,
`RuntimeError`) tear down cleanly.

```cpp
while (!platform->shouldClose()) {
    if (mais::Error error = engine.update(); mais::isFatal(error)) {
        break; // stop cleanly
    }
    if (mais::Error error = engine.render(); error != mais::Error::Ok) {
        if (mais::isRecoverable(error)) {
            continue; // swapchain was recreated, retry next frame
        }
        break; // fatal
    }
    engine.pollEvents();
    if (mais::isFatal(engine.getLastError())) {
        break; // e.g. XR_SESSION_STATE_LOSS_PENDING
    }
}
```

`pollEvents()` keeps returning the event vector; the reason a platform is
closing is queryable via `Engine::getLastError()` / `IPlatform::getLastError()`.

## Backend mappings

- `mapVkResult(VkResult)` — Vulkan codes (see `sources/Error.cpp`).
- `mapXrResult(XrResult)` and `mapSessionState(XrSessionState)` — OpenXR codes
  (see `sources/openxr/XrError.cpp`).
- GLFW errors are swept in `IDesktopPlatform::pollEvents` and mapped to
  `RuntimeError`.

## Init helpers

Resource-creation helpers (`createBuffer`, `createImage`, `transitionImageLayout`,
`copyBuffer`, `copyBufferToImage`) return `mais::Error`; `createImageView`
returns `mais::Result<VkImageView>`.
