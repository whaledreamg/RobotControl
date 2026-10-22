# 机器人综合控制系统

基于 PySide6 的机器人综合控制软件，支持 AGV 底盘控制和双机械臂控制。

## 功能特性

- **AGV 控制**
  - 速度调节（遥控AGV）
  - 方向控制（前/后/左/右/停止）
  - 导航控制（站点下拉选择 + 开始/暂停/取消）
  - 急停
  - 位置、状态、电量实时显示

- **机械臂控制**
  - 双臂使能（LeftArm/RightArm）
  - 运行速度调节
  - 工程列表（下拉选择）
  - 工程控制（开始/暂停/继续/停止）
  - 关节角、末端位姿实时显示

- **辅助设备**
  - 夹爪控制（打开/闭合/停止，力度可调）
  - 升降柱控制（高度/限位/复位）

- **系统功能**
  - 统一IP配置 + 可配置端口
  - 数据持久化（QSettings）
  - 实时状态推送（10Hz）

## 技术栈

- **UI 框架**: PySide6
- **机械臂通信**: WebSocket (`websocket-client`)
- **AGV 通信**: TCP Socket + 自定义16字节帧头
- **配置管理**: QSettings

## 项目结构

```
RobotControl/
├── main.py                      # 程序入口
├── requirements.txt             # 依赖列表
├── install.sh                   # 安装脚本
├── config/
│   └── config.json             # 默认配置
├── ui/
│   ├── main_window.py          # 主窗口
│   ├── settings_dialog.py      # 设置对话框
│   ├── agv_control.py          # AGV控制模块
│   ├── arm_control.py          # 机械臂控制模块
│   ├── gripper_control.py      # 夹爪控制模块
│   └── bollard_control.py      # 升降柱控制模块
├── communication/
│   ├── agv_protocol.py         # AGV TCP 协议封装
│   ├── agv_tcp_client.py       # AGV TCP 客户端
│   └── arm_websocket.py        # 机械臂 WebSocket 客户端
├── core/
│   └── settings_manager.py     # QSettings 封装
└── resources/
    ├── icons/
    └── styles/
```

## 安装

```bash
# 方式1: 使用安装脚本
bash install.sh

# 方式2: 手动安装
pip3 install -r requirements.txt
```

## 运行

```bash
python3 main.py
```

## 默认配置

- **IP 地址**: 192.168.100.16
- **机械臂 WebSocket 端口**: 9000
- **左臂名称**: LeftArm
- **右臂名称**: RightArm
- **AGV 推送端口**: 19301
- **AGV 控制端口**: 19205
- **AGV 导航端口**: 19206
- **AGV 状态端口**: 19204
- **AGV 其他端口**: 19210
- **推送频率**: 10Hz (100ms)

## 通信协议

### 机械臂 (WebSocket)

- `robot/enable` - 上使能
- `robot/disable` - 下使能
- `device/setSpeedOverride` - 设置速度倍率
- `device/getSpeedOverride` - 获取速度倍率
- `project/listProjects` - 获取工程列表
- `project/run` - 运行工程
- `project/pause` - 暂停工程
- `project/resume` - 恢复工程
- `project/stop` - 停止工程
- `projectState/sub` - 订阅工程状态
- `robotState/sub` - 订阅机械臂状态

### AGV (TCP)

- 命令 2010: 开环运动控制
- 命令 2000: 停止运动
- 命令 3001-3003: 暂停/继续/取消导航
- 命令 3051: 开始导航
- 命令 6004: 软急停
- 端口 19301: 状态推送 (10Hz)

## 使用说明

1. **启动程序**: 运行 `python3 main.py`
2. **输入 IP**: 在顶部 IP 输入框输入机械臂和 AGV 的 IP（两者使用相同 IP）
3. **点击连接**: 建立 WebSocket（机械臂）和 TCP（AGV）连接
4. **控制设备**:
   - 使用 AGV 控制面板进行遥控或导航
   - 使用机械臂控制面板进行使能、速度设置、工程控制
5. **修改配置**: 点击右上角"⚙ 设置"按钮

## 数据持久化

所有配置都使用 QSettings 保存：
- 窗口位置和大小
- 上次连接的 IP
- 机械臂和 AGV 端口
- 速度、高度等参数

## 注意事项

- AGV 和机械臂使用相同的 IP 地址，但端口不同
- 速度修改实时生效
- 急停按钮会同时停止 AGV 和机械臂
- 状态推送频率默认为 10Hz
