# 项目交付清单

## 已完成

### 1. 项目结构
```
RobotControl/
├── main.py                          # ✅ 程序入口
├── requirements.txt                 # ✅ 依赖列表
├── install.sh                       # ✅ 安装脚本
├── README.md                        # ✅ 项目说明
├── config/
│   └── config.json                  # ✅ 默认配置
├── ui/
│   ├── __init__.py                  # ✅ 模块初始化
│   ├── main_window.py               # ✅ 主窗口
│   ├── settings_dialog.py           # ✅ 设置对话框
│   ├── agv_control.py               # ✅ AGV控制模块
│   ├── arm_control.py               # ✅ 机械臂控制模块
│   ├── gripper_control.py           # ✅ 夹爪控制模块
│   └── bollard_control.py           # ✅ 升降柱控制模块
├── communication/
│   ├── __init__.py                  # ✅ 模块初始化
│   ├── agv_protocol.py              # ✅ AGV TCP协议
│   ├── agv_tcp_client.py            # ✅ AGV TCP客户端
│   └── arm_websocket.py             # ✅ 机械臂 WebSocket客户端
└── core/
    ├── __init__.py                  # ✅ 模块初始化
    └── settings_manager.py          # ✅ QSettings封装
```

### 2. 功能清单

| 功能 | 状态 | 说明 |
|------|------|------|
| 统一IP输入 | ✅ | 顶部IP输入框 + 连接按钮 |
| 默认IP | ✅ | 192.168.100.16 |
| IP可配置 | ✅ | 通过设置面板修改 |
| 左臂名称 | ✅ | LeftArm |
| 右臂名称 | ✅ | RightArm |
| 推送频率 | ✅ | 10Hz (100ms) |
| AGV导航站点 | ✅ | 下拉框 |
| 工程列表 | ✅ | 下拉框 |
| 数据持久化 | ✅ | QSettings |
| 机械臂使能 | ✅ | 双臂使能开关 |
| 机械臂速度 | ✅ | 滑块调节 |
| 双向控制 | ✅ | 前/后/左/右 + 停止 |
| 急停功能 | ✅ | AGV急停 |
| 工程控制 | ✅ | 开始/暂停/继续/停止 |
| 夹爪控制 | ✅ | 开/合/停止 + 力度 |
| 升降柱控制 | ✅ | 高度/限位/复位 |
| AGV状态推送 | ✅ | 10Hz实时显示 |
| 机械臂状态推送 | ✅ | 关节角/位姿/使能 |
| 清除异常 | ✅ | 按钮功能 |
| 设置面板 | ✅ | 标签页式设置 |

### 3. 通信协议

#### 机械臂 (WebSocket)
- ✅ `robot/enable` - 上使能
- ✅ `robot/disable` - 下使能
- ✅ `device/setSpeedOverride` - 设置速度倍率
- ✅ `device/getSpeedOverride` - 获取速度倍率
- ✅ `project/listProjects` - 获取工程列表
- ✅ `project/run` - 运行工程
- ✅ `project/pause` - 暂停工程
- ✅ `project/resume` - 恢复工程
- ✅ `project/stop` - 停止工程
- ✅ `projectState/sub` - 订阅工程状态
- ✅ `robotState/sub` - 订阅机械臂状态

#### AGV (TCP)
- ✅ 命令 2010 - 开环运动控制
- ✅ 命令 2000 - 停止运动
- ✅ 命令 3051 - 开始导航
- ✅ 命令 3001 - 暂停导航
- ✅ 命令 3002 - 继续导航
- ✅ 命令 3003 - 取消导航
- ✅ 命令 6004 - 软急停
- ✅ 端口 19301 - 状态推送 (10Hz)
- ✅ 端口 19205 - 控制
- ✅ 端口 19206 - 导航
- ✅ 端口 19204 - 状态查询
- ✅ 端口 19210 - 其他

## 使用方法

### 1. 安装依赖
```bash
cd /home/gxh/workspace/project/RobotControl
bash install.sh
```

### 2. 运行程序
```bash
python3 main.py
```

### 3. 操作流程
1. 顶部IP输入框输入 IP（默认 192.168.100.16）
2. 点击"连接"按钮
3. 等待连接成功
4. 开始使用

## 技术栈

- **Python 3.8+**
- **PySide6** - UI框架
- **websocket-client** - 机械臂通信
- **QSettings** - 配置持久化

## 协议帧格式

### AGV TCP 协议 (16字节帧头)
```
| 同步头(1B) | 版本(1B) | 序号(2B) | 数据长度(4B) | 命令号(2B) | 保留(6B) | JSON数据 |
   0x5A       0x01      0-65535   变长          命令号      -
```

### 机械臂 WebSocket 协议
```json
{
    "id": 1,
    "ty": "动作名",
    "db": {}
}
```

## 数据持久化

所有配置使用 QSettings 保存：
- IP地址、端口号
- 机械臂名称（左/右臂）
- 推送频率
- 速度、高度等参数

## 测试结果

### AGV协议测试
- ✅ 帧构建：命令2010，长度65字节
- ✅ 帧构建：命令2000，null参数，长度20字节
- ✅ 帧解析：成功还原命令号、序号和JSON

### QSettings测试
- ✅ 默认IP：192.168.100.16
- ✅ 默认WebSocket端口：9000
- ✅ 默认左臂名称：LeftArm
- ✅ 默认右臂名称：RightArm
- ✅ 默认AGV推送端口：19301
- ✅ 默认推送间隔：100ms

## 注意事项

1. AGV 和机械臂使用相同 IP，但端口不同
2. 速度修改实时生效
3. 急停会同时停止 AGV 运动
4. 状态推送频率默认为 10Hz
5. 关闭程序时自动断开连接
