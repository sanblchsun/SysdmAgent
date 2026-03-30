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
logger.setLevel(logging.DEBUG)

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
    logger.info(f">>> Agent {agent_id} connecting...")
    await ws.accept()
    agents[agent_id] = ws
    logger.info(f">>> Agent {agent_id} CONNECTED. Viewers: {list(viewers.keys())}")

    try:
        while True:
            try:
                logger.debug(f">>> Agent {agent_id} waiting for data...")
                data = await asyncio.wait_for(ws.receive(), timeout=30.0)
                logger.debug(f">>> Agent {agent_id} received data type: {data.get('type')}")
            except asyncio.TimeoutError:
                logger.debug(f">>> Agent {agent_id} timeout waiting for data")
                continue
            except Exception as e:
                logger.error(f">>> Error receiving from {agent_id}: {e}")
                break

            if data["type"] == "websocket.disconnect":
                logger.info(f">>> Agent {agent_id} disconnected by client")
                break

            if data["type"] == "websocket.receive":
                logger.info(f">>> Agent {agent_id} websocket.receive keys: {list(data.keys())}")
                
                if "text" in data:
                    text_data = data["text"]
                    logger.info(f">>> Agent {agent_id} TEXT: {text_data[:100]}")
                    
                    viewer = viewers.get(agent_id)
                    if viewer:
                        try:
                            await viewer.send_text(text_data)
                            logger.info(f">>> Forwarded TEXT to viewer {agent_id}")
                        except Exception as e:
                            logger.error(f">>> Error sending text to viewer: {e}")
                            viewers.pop(agent_id, None)
                    else:
                        logger.warning(f">>> No viewer for {agent_id}, text dropped")
                            
                elif "bytes" in data:
                    bytes_data = data["bytes"]
                    logger.info(f">>> Agent {agent_id} BINARY: {len(bytes_data)} bytes")
                    
                    viewer = viewers.get(agent_id)
                    if viewer:
                        try:
                            await viewer.send_bytes(bytes_data)
                            logger.info(f">>> Forwarded BINARY to viewer {agent_id}: {len(bytes_data)} bytes")
                        except Exception as e:
                            logger.error(f">>> Error sending binary to viewer: {e}")
                            viewers.pop(agent_id, None)
                    else:
                        logger.warning(f">>> No viewer for {agent_id}, binary dropped: {len(bytes_data)} bytes")
                else:
                    logger.warning(f">>> Agent {agent_id} unknown data keys: {list(data.keys())}")

    except WebSocketDisconnect:
        logger.info(f">>> Agent {agent_id} WebSocketDisconnect")
    except Exception as e:
        logger.exception(f">>> Error in agent socket {agent_id}: {e}")
    finally:
        agents.pop(agent_id, None)


@app.websocket("/ws/viewer/{agent_id}")
async def viewer_ws(ws: WebSocket, agent_id: str):
    logger.info(f">>> Viewer {agent_id} connecting...")
    await ws.accept()

    old = viewers.get(agent_id)
    if old:
        logger.info(f">>> Replacing old viewer for {agent_id}")
        try:
            await old.close()
        except:
            pass

    viewers[agent_id] = ws
    logger.info(f">>> Viewer {agent_id} CONNECTED. Agents: {list(agents.keys())}")

    try:
        while True:
            try:
                data = await asyncio.wait_for(ws.receive(), timeout=60.0)
            except asyncio.TimeoutError:
                continue
            except Exception as e:
                logger.error(f">>> Error receiving from viewer {agent_id}: {e}")
                break

            if data["type"] == "websocket.disconnect":
                logger.info(f">>> Viewer {agent_id} disconnected by client")
                break

            if data["type"] == "websocket.receive":
                text_data = data.get("text")
                if text_data:
                    logger.info(f">>> Viewer {agent_id} TEXT: {text_data[:100]}")
                    
                    agent = agents.get(agent_id)
                    if agent:
                        try:
                            await agent.send_text(text_data)
                        except Exception as e:
                            logger.error(f">>> Error sending to agent: {e}")

    except WebSocketDisconnect:
        logger.info(f">>> Viewer {agent_id} WebSocketDisconnect")
    except Exception as e:
        logger.exception(f">>> Error in viewer socket {agent_id}: {e}")
    finally:
        viewers.pop(agent_id, None)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
