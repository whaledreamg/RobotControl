# 机器人综合控制系统

基于 **Qt 6 (C++17)** 的机器人综合控制软件，支持 AGV 底盘、双机械臂、升降柱、夹爪与 Revo2 灵巧手控制。
可编译为桌面程序，也可打包为 Android 应用。

## 功能特性

- **AGV 控制**
  - 速度调节（遥控 AGV）
  - 方向控制（前/后/左/右/停止）
  - 导航控制（站点下拉选择 + 开始/暂停/取消）
  - 急停
  - 位置、状态、电量实时显示

- **机械臂控制**
  - 单连接管理双臂（臂名仅为报文字段）
  - 双臂使能（LeftArm/RightArm）
  - 运行速度调节
  - 工程列表（下拉选择）
  - 工程控制（开始/暂停/继续/停止）
  - 力矩拖动（`robot/drag`）
  - 关节角、末端位姿实时显示
  - 清除驱动器故障（`robot/clearServoError`）

- **Revo2 灵巧手**
  - 6 自由度独立微调（DOF0~DOF5）
  - 预设手势（一把抓 / 释放 / 半抓 / 两指捏 / 点赞 / 指点）
  - 实时位置轮询回显
  - Modbus RTU 调试通道（自由拼报文）

- **辅助设备**
  - 夹爪控制（打开/闭合/停止，力度可调）
  - 升降柱控制（高度/限位/点动/复位）

- **系统功能**
  - 统一 IP 配置 + 可配置端口
  - 数据持久化（QSettings）
  - 实时状态推送（10Hz）

## 技术栈

| 项目 | 说明 |
|------|------|
| UI 框架 | Qt 6 Widgets（QSS 主题，兼容移动端布局） |
| 构建 | qmake |
| 语言 | C++17 |
| 机械臂通信 | QWebSocket（`QtWebSockets`） |
| AGV 通信 | QTcpSocket + 自定义 16 字节帧头 |
| 配置管理 | QSettings |
| 视图集成 | QWebEngineView（桌面）/ Qt WebView（Android） |

## 项目结构

```
RobotControl/
├── main.cpp                       # 程序入口
├── mainwindow.{h,cpp,ui}          # 主窗口
├── RobotControl.pro               # qmake 工程文件
├── common/                        # 全局常量、类型定义
│   ├── constants.h                # 默认 IP / 端口 / 限值
│   ├── types.h
│   └── global.{h,cpp}
├── communication/                 # 通信层
│   ├── armwebsocket.{h,cpp}       # 机械臂 WebSocket 客户端
│   ├── agvtcpclient.{h,cpp}       # AGV TCP 客户端
│   ├── agvprotocol.{h,cpp}        # AGV TCP 协议封装
│   ├── liftingcolumn.{h,cpp}      # 升降柱客户端
│   └── revo2hand.{h,cpp}          # Revo2 灵巧手客户端
├── core/
│   ├── settingsmanager.{h,cpp}    # QSettings 封装
│   └── qssmanager.{h,cpp}         # QSS 主题加载
├── dialogs/
│   ├── handviewdialog.{h,cpp}     # 灵巧手 View 弹窗（5 个 Tab）
│   ├── settingsdialog.{h,cpp,ui}  # 设置对话框
│   ├── mapinfodialog.{h,cpp}      # 地图信息对话框
│   ├── businesstestpanel.{h,cpp}  # 业务测试面板
│   └── webviewdialog.{h,cpp}      # WebView 对话框
├── ui/
│   ├── handpanel.{h,cpp}          # 灵巧手面板
│   └── messagewidget.{h,cpp}      # 消息提示组件
├── resources/
│   ├── icons/                     # 图标资源
│   ├── styles/                    # QSS 样式表
│   └── qml/                       # Android WebView 桥接 QML
└── config/
    └── config.json                # 默认配置
```

## 环境要求

- **Qt 6.8.3**（需包含 `WebSockets` 模块；桌面端另需 `WebEngineWidgets`）
- **编译器**：支持 C++17 的 GCC 9+ / Clang 10+ / MSVC 2019+
- **Android**（可选）：Qt 6 Android 工具链 + NDK

## 编译

```bash
cd /home/gxh/workspace/project/RobotControl

# 生成 Makefile（注意指定 Qt 6 的 qmake，勿用系统默认的 Qt 5）
/path/to/Qt/6.8.3/gcc_64/bin/qmake RobotControl.pro

# 编译
make -j$(nproc)
```

编译产物为 `./RobotControl`。

### Android 打包

工程已内置 `android/` 目录与 Gradle 配置：

```bash
# 需先配置 ANDROID_NDK_ROOT 等环境变量，并在 .pro 中修正 android_openssl.pri 路径
/path/to/Qt/6.8.3/gcc_64/bin/qmake RobotControl.pro
make -j$(nproc)
```

## 默认配置

| 项目 | 默认值 |
|------|--------|
| 机械臂 IP | 192.168.100.16 |
| AGV IP | 192.168.100.101 |
| 机械臂 WebSocket 端口 | 9000 |
| 左臂名称 | LeftArm |
| 右臂名称 | RightArm |
| AGV 推送端口 | 19301 |
| AGV 控制端口 | 19205 |
| AGV 导航端口 | 19206 |
| AGV 状态端口 | 19204 |
| AGV 其他端口 | 19210 |
| 推送频率 | 10Hz (100ms) |

## 通信协议

### 机械臂（WebSocket）

报文信封：`{ "id": n, "ty": "动作名", "db": {...} }`

- 订阅类报文（`robotState/sub`、`projectState/sub`、`liftingColumnState/sub`）的订阅字段放在**顶层**，不在 `db` 内
- 订阅主题必须使用 `robot/list` 返回的真实臂名，写错会被拒（25004）

| 动作 | 说明 |
|------|------|
| `robot/enable` / `robot/disable` | 上/下使能 |
| `robot/drag` | 进入力矩拖动模式 |
| `robot/status` | 关节角、位姿、使能状态 |
| `robot/list` | 查询真实臂名 |
| `robot/clearServoError` | 驱动器故障复位 |
| `device/setSpeedOverride` | 设置速度倍率 |
| `device/getSpeedOverride` | 获取速度倍率 |
| `project/listProjects` | 工程列表 |
| `project/run` / `pause` / `resume` / `stop` | 工程控制 |
| `project/status` | 工程状态 |
| `liftingColumn/list` / `getState` / `moveTo` / `jogStart` | 升降柱 |

### Revo2 灵巧手（RS485 / Modbus RTU）

经由 WebSocket 桥接转发，实际走 `rs485/*` 系列报文：

| 动作 | 说明 |
|------|------|
| `rs485/list` | 列出可用通道，解析从站对应的 channel |
| `rs485/getStatus` | 查询通道占用情况（被夹爪协议占用时需先释放） |
| `peripheral/setDeviceEnabled` | 停用占用通道的夹爪协议 |
| `rs485/init` | 初始化通道（115200 / 1 停位 / 无校验） |
| `rs485/write` | 写入 Modbus 帧 |
| `rs485/read` | 读取 Modbus 响应 |

Modbus 寄存器：读位置 `0x07D0`、写单 DOF `0x041C`、写全部 DOF `0x03F2`。

### AGV（TCP）

16 字节帧头：

```
| 同步头(1B) | 版本(1B) | 序号(2B) | 数据长度(4B) | 命令号(2B) | 保留(6B) | JSON数据 |
   0x5A       0x01      0-65535   变长          命令号      -
```

- 命令 2010：开环运动控制
- 命令 2000：停止运动
- 命令 3051：开始导航
- 命令 3001 / 3002 / 3003：暂停 / 继续 / 取消导航
- 命令 6004：软急停

## 使用说明

1. **启动程序**：运行 `./RobotControl`
2. **输入 IP**：在顶部 IP 输入框输入机械臂和 AGV 的 IP（两者默认不同）
3. **点击连接**：建立 WebSocket（机械臂）与 TCP（AGV）连接
4. **初始化灵巧手**：点击「连接双手」完成 `rs485/list` → 通道释放 → `rs485/init`
5. **控制设备**：
   - AGV 面板：遥控或导航
   - 机械臂面板：使能、速度、工程控制
   - 灵巧手面板：预设手势；「View」弹窗内可逐指微调
6. **修改配置**：点击右上角「⚙ 设置」按钮

## 线程模型

`QWebSocket` 的信号在其所属线程派发，因此：

- `ArmWebSocket` 所在线程负责收发与响应派发
- 跨线程的同步调用（`sendAndWaitSafe`）采用「发送排回 socket 线程 + **在调用方线程用条件变量等待**」的方式，**不会阻塞 UI 线程**
- 需要长耗时通信的操作（灵巧手初始化、预设下发、Modbus 调试）一律在 `QThreadPool` 中执行，结果回投 UI 线程

## 数据持久化

所有配置使用 QSettings 保存：窗口位置和大小、上次连接的 IP、各端口、速度与高度等参数。

## 注意事项

- AGV 与机械臂使用不同 IP，端口也不相同
- 速度修改实时生效
- 急停按钮会同时停止 AGV 和机械臂
- 状态推送频率默认为 10Hz
- 关闭程序时自动断开连接
