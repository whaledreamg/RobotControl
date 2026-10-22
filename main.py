"""
机器人综合控制系统
"""
import sys
import os
from PySide6.QtWidgets import QApplication

from ui.main_window import MainWindow
from ui.theme import apply_app_style


def main():
    """主函数"""
    app = QApplication(sys.argv)

    # 设置应用信息
    app.setApplicationName("RobotControl")
    app.setApplicationVersion("1.0.0")
    app.setOrganizationName("RobotControl")

    # 全局简约果冻风格（在创建主窗口之前调）
    apply_app_style(app)

    # 调试开关：
    #   ARM_DEBUG_PUSH=1   打印机械臂状态推送全链路日志
    #   AGV_DEBUG_DUMP=1   打印 AGV 下行报文完整 dump
    # 也支持命令行 --debug-arm / --debug-agv
    if "--debug-arm" in sys.argv:
        os.environ["ARM_DEBUG_PUSH"] = "1"
        print("[Debug] 机械臂推送日志已开启")
    if "--debug-agv" in sys.argv:
        os.environ["AGV_DEBUG_DUMP"] = "1"
        print("[Debug] AGV 报文 dump 已开启")

    # 创建并显示主窗口
    window = MainWindow()
    window.show()

    # 运行应用
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
