# rmm/main.py
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.templating import Jinja2Templates
import os
import asyncio
import logging
from typing import Dict

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger("rmm")

app = FastAPI(title="RMM Signaling Server")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
templates = Jinja2Templates(directory=os.path.join(BASE_DIR, "templates"))

agents: Dict[str, WebSocket] = {}
viewers: Dict[str, WebSocket] = {}


@app.get("/")
async def index(request: Request):
    return templates.TemplateResponse("viewer.html", {"request": request})


@app.get("/status")
async def status():
    return {
        "agents": list(agents.keys()),
        "viewers": list(viewers.keys())
    }


@app.websocket("/ws/agent/{agent_id}")
async def agent_ws(ws: WebSocket, agent_id: str):
    logger.info(f"=== Agent {agent_id} connecting...")
    await ws.accept()
    agents[agent_id] = ws
    logger.info(f"=== Agent {agent_id} CONNECTED")

    try:
        while True:
            try:
                logger.info(f"Agent {agent_id}: calling receive()...")
                raw = await ws._ws.accepted
                data = await ws.receive()
                logger.info(f"Agent {agent_id}: received data type={data.get('type')}, keys={list(data.keys())}")
            except Exception as e:
                logger.error(f"Agent {agent_id}: receive error: {e}")
                break

            if data["type"] == "websocket.disconnect":
                logger.info(f"Agent {agent_id}: disconnected")
                break

            if data["type"] == "websocket.receive":
                if "text" in data:
                    text_data = data["text"]
                    logger.info(f"Agent {agent_id}: TEXT [{len(text_data)} bytes]: {text_data[:50]}")
                    viewer = viewers.get(agent_id)
                    if viewer:
                        await viewer.send_text(text_data)
                        logger.info(f"Forwarded to viewer")
                    else:
                        logger.warning(f"No viewer connected")
                        
                elif "bytes" in data:
                    bytes_data = data["bytes"]
                    logger.info(f"Agent {agent_id}: BYTES [{len(bytes_data)} bytes]")
                    viewer = viewers.get(agent_id)
                    if viewer:
                        await viewer.send_bytes(bytes_data)
                        logger.info(f"Forwarded {len(bytes_data)} bytes to viewer")
                    else:
                        logger.warning(f"No viewer connected, dropped {len(bytes_data)} bytes")

    except WebSocketDisconnect:
        logger.info(f"Agent {agent_id}: WebSocketDisconnect")
    except Exception as e:
        logger.exception(f"Agent {agent_id}: exception: {e}")
    finally:
        agents.pop(agent_id, None)


@app.websocket("/ws/viewer/{agent_id}")
async def viewer_ws(ws: WebSocket, agent_id: str):
    logger.info(f"=== Viewer {agent_id} connecting...")
    await ws.accept()
    
    old = viewers.get(agent_id)
    if old:
        try:
            await old.close()
        except:
            pass

    viewers[agent_id] = ws
    logger.info(f"=== Viewer {agent_id} CONNECTED")

    try:
        while True:
            try:
                data = await ws.receive()
            except Exception as e:
                logger.error(f"Viewer {agent_id}: receive error: {e}")
                break

            if data["type"] == "websocket.disconnect":
                break

            if data["type"] == "websocket.receive":
                text_data = data.get("text")
                if text_data:
                    logger.info(f"Viewer {agent_id}: TEXT: {text_data[:50]}")
                    agent = agents.get(agent_id)
                    if agent:
                        await agent.send_text(text_data)

    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.exception(f"Viewer {agent_id}: exception: {e}")
    finally:
        viewers.pop(agent_id, None)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
