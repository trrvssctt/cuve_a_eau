"""
orchestrator.py — Boucle principale de l'agent CuveGuard (point d'entree).

Cycle (toutes les `intervalle_s` secondes) :
  1. tb_rest  : lit waterLevelPct, manualMode, pumpOn sur le device ESP32
  2. control  : decide START / STOP / HOLD (seuils 30 % / 90 %, hysteresis)
  3. tb_rest  : envoie la RPC setPump si necessaire (jamais en mode manuel)
  4. tb_client: publie la telemetrie agent (cles imposees par le sujet)

Lancement :
    pip install -r requirements.txt
    cp config/thingsboard_config.example.json config/thingsboard_config.json
    # remplir config/thingsboard_config.json
    python -m agents.orchestrator
"""

import json
import sys
import time
from pathlib import Path

import requests

from agents.control_agent import ControlAgent
from agents.tb_client import TbClient
from agents.tb_rest import TbRest

CONFIG_FILE = (Path(__file__).resolve().parent.parent
               / "config" / "thingsboard_config.json")


def charger_config() -> dict:
    if not CONFIG_FILE.exists():
        print(f"ERREUR : {CONFIG_FILE} introuvable.")
        print("Copiez thingsboard_config.example.json vers "
              "thingsboard_config.json et remplissez-le.")
        sys.exit(1)
    with open(CONFIG_FILE, encoding="utf-8") as f:
        return json.load(f)


def main() -> None:
    cfg = charger_config()

    rest = TbRest(cfg["tb_host"], cfg["tb_username"], cfg["tb_password"],
                  cfg["esp32_device_id"])
    rest.login()

    agent_mqtt = TbClient(cfg["tb_host"], cfg["agent_access_token"],
                          cfg.get("tb_mqtt_port", 1883))
    agent_mqtt.connect()

    control = ControlAgent(cfg.get("seuil_start", 30.0),
                           cfg.get("seuil_stop", 90.0))
    intervalle = cfg.get("intervalle_s", 10)

    print(f"[ORCHESTRATOR] Demarre — seuils "
          f"{control.seuil_start:.0f}%/{control.seuil_stop:.0f}%, "
          f"cycle {intervalle}s. Ctrl+C pour arreter.")

    # Gestion fiable de la commande pompe :
    #   - `last_pump_command` = etat commande, en attente de confirmation par
    #     la telemetrie (None = pas de commande en attente).
    #   - on renvoie la RPC si l'etat n'est toujours pas confirme apres
    #     `RENVOI_RPC_S` (RPC one-way non garantie : une commande peut se
    #     perdre pendant une micro-deconnexion MQTT), sans pour autant
    #     spammer a chaque cycle.
    last_pump_command = None
    last_command_time = 0.0
    RENVOI_RPC_S = 6.0

    try:
        while True:
            try:
                tele = rest.lire_telemetrie(
                    ["waterLevelPct", "manualMode", "pumpOn"])

                if "waterLevelPct" not in tele:
                    print("[ORCHESTRATOR] Pas encore de telemetrie ESP32...")
                    time.sleep(intervalle)
                    continue

                level = float(tele["waterLevelPct"])
                manual = str(tele.get("manualMode", "false")).lower() == "true"
                pump_on = str(tele.get("pumpOn", "false")).lower() == "true"

                command_sent = False

                if manual:
                    # Mode manuel : l'agent observe mais ne commande jamais.
                    # On oublie la derniere commande auto : au retour en auto,
                    # la decision repartira de l'etat reel du device.
                    decision, auto_mode = "HOLD", False
                    last_pump_command = None
                    print(f"[ORCHESTRATOR] niveau={level:.0f}% | "
                          f"MODE MANUEL -> HOLD")
                else:
                    auto_mode = True
                    # On reconcilie notre memoire avec l'etat reel : si la
                    # telemetrie confirme l'etat de la pompe, la commande est
                    # bien passee, on peut re-commander librement plus tard.
                    if last_pump_command is not None and pump_on == last_pump_command:
                        last_pump_command = None

                    d = control.decider(level, pump_on)
                    decision = d.decision
                    if d.commande is not None:
                        nouvelle = d.commande != last_pump_command
                        non_confirmee = (
                            time.monotonic() - last_command_time > RENVOI_RPC_S)
                        # Envoi si c'est une nouvelle commande, ou reemission
                        # si la precedente n'a toujours pas ete confirmee.
                        if nouvelle or non_confirmee:
                            rest.envoyer_rpc("setPump", d.commande)
                            last_pump_command = d.commande
                            last_command_time = time.monotonic()
                            command_sent = True
                    print(f"[ORCHESTRATOR] niveau={level:.0f}% "
                          f"pompe={'ON' if pump_on else 'OFF'} -> {decision}")

                # Telemetrie agent : cles imposees par le sujet
                agent_mqtt.publier_telemetrie({
                    "agentDecision": decision,
                    "observedLevelPct": level,
                    "pumpCommandSent": command_sent,
                    "autoMode": auto_mode,
                })

            except requests.RequestException as e:
                print(f"[ERREUR] Reseau/API : {e}")

            time.sleep(intervalle)

    except KeyboardInterrupt:
        print("\n[ORCHESTRATOR] Arret demande.")
    finally:
        agent_mqtt.disconnect()


if __name__ == "__main__":
    main()
