"""
tb_client.py — Publication MQTT de la telemetrie de l'agent.

Role : publier les cles imposees (agentDecision, observedLevelPct,
pumpCommandSent, autoMode) sur le device "agent" de ThingsBoard.
(Equivalent du tb_client.py du projet pompe-meteo-ml.)
"""

import json

import paho.mqtt.client as mqtt


class TbClient:
    """Client MQTT du device agent (auth par access token)."""

    def __init__(self, host: str, token: str, port: int = 1883):
        self.host = host
        self.port = port
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="agent-cuveguard",
        )
        self.client.username_pw_set(token)

    def connect(self) -> None:
        self.client.connect(self.host, self.port, keepalive=60)
        self.client.loop_start()
        print("[TB-MQTT] Agent connecte")

    def publier_telemetrie(self, telemetrie: dict) -> None:
        payload = json.dumps(telemetrie)
        self.client.publish("v1/devices/me/telemetry", payload, qos=1)

    def disconnect(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()
