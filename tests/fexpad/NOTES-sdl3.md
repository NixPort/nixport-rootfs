# SDL3 侧的前提（原生轨虚拟手柄垫片）


## 1. 版本与库名

- runtime 的 SDL3 来自构建机的 `pkg-config sdl3`（gow3_windows `packaging/arm-linux/build.sh`
  就是这么链的）；本机（macOS/Homebrew）实测为 **3.4.18**，
  库文件 `libSDL3.0.dylib`（安装名）与 `libSDL3.dylib`（链接名）。
- Linux 侧为 `libSDL3.so.0`（soname）与 `libSDL3.so`（开发软链）。
- **垫片按 soname 依次尝试**：`libSDL3.so.0` → `libSDL3.so` → `libSDL3.0.dylib` →
  `libSDL3.dylib`。动态加载器按 soname 去重，所以垫片拿到的就是**游戏自己那份 SDL**。

## 2. 需要的 API（SDL 3.2+ 全部具备）

| 用途 | API | 备注 |
|---|---|---|
| 初始化手柄子系统 | `SDL_InitSubSystem(SDL_INIT_JOYSTICK)` | 游戏随后 `SDL_Init(SDL_INIT_GAMEPAD)` 不冲突 |
| 登记虚拟手柄 | `SDL_AttachVirtualJoystick(desc)` | 3.2.0 起 |
| 打开手柄（setter 的前提） | `SDL_OpenJoystick(instance_id)` | setter 要求已打开 |
| 写轴 | `SDL_SetJoystickVirtualAxis(joystick, axis, value)` | 任意线程可调 |
| 写键 | `SDL_SetJoystickVirtualButton(joystick, button, down)` | 同上 |
| 注销虚拟手柄 | `SDL_DetachVirtualJoystick(instance_id)` | 游戏侧随即收到移除事件 |
| 读身份 | `SDL_GetJoystickNameForID` / `SDL_GetJoystickVendorForID` / `SDL_GetJoystickProductForID` | 注意是 `...ForID`，没有 `...ID` 版本 |

## 3. 类型为手柄时 SDL 自动生成标准映射

`SDL_VirtualJoystickDesc.type = SDL_JOYSTICK_TYPE_GAMEPAD` 时：

- SDL 自动推导 `button_mask` / `axis_mask`（`src/joystick/virtual/SDL_virtualjoystick.c:198-232`）：
  `naxes>=2` → 左摇杆，`>=4` → 右摇杆，`>=6` → 两个扳机；`button_mask` 为前 `nbuttons` 位；
- 驱动提供**标准手柄映射** `VIRTUAL_JoystickGetGamepadMapping`（同文件 `:793` 起），
  因此 `SDL_IsGamepad()` 成立、`SDL_GetGamepads()` 能枚举到它，游戏可用整个 SDL 手柄 API；
- **按钮索引即 `SDL_GamepadButton` 的标准顺序**（SOUTH, EAST, WEST, NORTH, BACK, GUIDE,
  START, LEFT_STICK, RIGHT_STICK, LEFT_SHOULDER, RIGHT_SHOULDER, DPAD_UP, DPAD_DOWN,
  DPAD_LEFT, DPAD_RIGHT, …），轴索引即 `SDL_GamepadAxis` 顺序
  （LEFTX, LEFTY, RIGHTX, RIGHTY, LEFT_TRIGGER, RIGHT_TRIGGER）；
- 该驱动默认编译进 SDL3：`SDL_HIDAPI` 默认 ON（`CMakeLists.txt:391`）→
  `SDL_VIRTUAL_JOYSTICK` 默认 ON（`:395` → `:1466`）。

## 4. 量程：扳机要用 −32768..32767，不是 0..32767

SDL 的手柄扳机轴是 0..32767，但那是**归一化之后**的值：

- evdev 路径里 `AxisCorrect()`（`src/joystick/linux/SDL_sysjoystick.c:1786`）把设备的
  0..255 映射到 SDL 的轴范围 **−32768（松手）..32767（满）**；
- 虚拟手柄没有这一步，所以垫片必须**自己**把 0..255 放到 −32768..32767；
- SDL 的手柄映射再把 −32768..32767 归一化成 0..32767 给游戏
  （`src/joystick/SDL_gamepad.c:286-292`）。

实测（host 测试）：设备的 128 → 垫片写 128 → 游戏看到 16447（期望 16448，浮点归一化的
1 LSB 误差）→ 因此断言用容差。

摇杆不受影响：设备的 ±32768..32767 与 SDL 轴范围一致，直传即可。

## 5. 描述符的 version 字段

`SDL_AttachVirtualJoystick` 只在 `desc->version < sizeof(SDL_VirtualJoystickDesc)` 时报错
（`src/joystick/virtual/SDL_virtualjoystick.c:145`，注释写明"Update this to handle older
versions of this interface"）→ 垫片按 SDL 3.4/3.5 的布局镜像该结构并填
`version = sizeof(自己的结构)` 即可；结构布局在 3.2..3.5 之间未变
（`ntouchpads`/`nsensors` 自 3.2.0 起就在）。

## 6. 构造阶段调用 SDL 的安全性（已实测）

垫片在 `__attribute__((constructor))` 里 `dlopen` SDL3 并 `SDL_InitSubSystem(SDL_INIT_JOYSTICK)`：
host 测试（macOS，构造阶段即初始化）通过，游戏侧 `SDL_GetGamepads()` 能看到垫片登记的
虚拟手柄 → **垫片与游戏共享同一份 SDL 实例**，构造阶段初始化 SDL 安全。
