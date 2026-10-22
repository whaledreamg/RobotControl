"""运行时配置加载，复用原 RobotControl/config/ 的 settings。"""
from __future__ import annotations

import os
import sys
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict


# 把仓库根的 communication / core 加进 sys.path，让后端可以直接
# `from communication.agv_tcp_client import AGVTcpClient`
class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        extra="ignore",
    )

    HOST: str = "0.0.0.0"
    PORT: int = 8000

    # 复用现有通信层：相对于 backend/ 的路径
    ROBOT_CONTROL_ROOT: str = "../../"
    ROBOT_CONTROL_SRC_DIRS: str = "communication,core"
    USE_MOCK_DEVICES: bool = True

    def wire_legacy_path(self) -> None:
        """把原 RobotControl 的子目录塞进 sys.path，零成本复用通信层。"""
        root = Path(__file__).resolve().parents[2] / self.ROBOT_CONTROL_ROOT
        for sub in self.ROBOT_CONTROL_SRC_DIRS.split(","):
            p = (root / sub.strip()).resolve()
            if p.exists() and str(p) not in sys.path:
                sys.path.insert(0, str(p))


settings = Settings()
settings.wire_legacy_path()
