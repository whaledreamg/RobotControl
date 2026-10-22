"""FastAPI 入口。"""
from __future__ import annotations

from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.api.routes import router
from app.core.config import settings


@asynccontextmanager
async def lifespan(app: FastAPI):
    # 启动钩子：连接默认设备、加 mock 状态都行
    yield


app = FastAPI(title="RobotControl Exhibit Backend", lifespan=lifespan)

# 展厅单机场景放行所有来源即可；正式部署收紧
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(router)


@app.get("/")
async def root() -> dict:
    return {
        "service": "robotcontrol-exhibit",
        "version": "0.1.0",
        "use_mock": settings.USE_MOCK_DEVICES,
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "app.main:app",
        host=settings.HOST,
        port=settings.PORT,
        reload=True,
    )
