# CuveGuard — Surveillance d'un réservoir d'eau avec remplissage automatique

Projet IoT Master 1 IA — DIT.

Un ESP32 (Wokwi) mesure le niveau d'eau d'une cuve (HC-SR04) et la température/humidité (DHT22), publie la télémétrie vers ThingsBoard en MQTT, et signale localement un niveau critique (LED rouge + buzzer). Un agent Python lit le niveau sur ThingsBoard et pilote la pompe (relais + LED) par RPC selon les seuils 30 % / 90 % avec hystérésis. Le dashboard ThingsBoard permet aussi le pilotage manuel.

## Architecture du dépôt

```
├── README.md
├── requirements.txt              requests, paho-mqtt
├── .gitignore                    secrets.h, thingsboard_config.json, __pycache__
│
├── agents/                       ── AGENT PYTHON (PC) ────────────
│   ├── orchestrator.py           Boucle principale : point d'entrée
│   ├── control_agent.py          Décide START/STOP/HOLD (dataclass PumpDecision)
│   ├── tb_rest.py                REST → lit la télémétrie ESP32 + envoie les RPC
│   └── tb_client.py              MQTT → publie la télémétrie de l'agent
│
├── config/
│   └── thingsboard_config.example.json   host, tokens, seuils 30/90, intervalle
│       (→ copier en thingsboard_config.json, gitignoré)
│
├── wokwi/                        ── FIRMWARE ESP32 ───────────────
│   ├── sketch.ino                HC-SR04 + DHT22 + pompe + alerte, MQTT + RPC
│   ├── diagram.json              Câblage du simulateur
│   ├── libraries.txt             DHT sensor library for ESPx, PubSubClient, ArduinoJson
│   └── secrets.example.h         Token TB (→ copier en secrets.h, gitignoré)
│
└── dashboard/
    └── cuveguard_dashboard.json  Dashboard à importer dans ThingsBoard
```

## Brochage (ESP32)

| Broche | Composant |
|---|---|
| 5 | HC-SR04 TRIG |
| 17 | HC-SR04 ECHO |
| 18 | DHT22 data |
| 26 | Relais IN (pompe) |
| 25 | LED rouge (alerte niveau bas) |
| 27 | Buzzer (niveau critique) |

## Calcul du niveau

Le HC-SR04 est fixé au sommet de la cuve et mesure la distance jusqu'à la surface :

```
niveau (%) = (DIST_VIDE − distance) / (DIST_VIDE − DIST_PLEINE) × 100
```

avec `DIST_VIDE = 380 cm` (0 %) et `DIST_PLEINE = 20 cm` (100 %). En simulation, cliquer sur le HC-SR04 pendant l'exécution et déplacer le slider de distance pour faire varier le niveau.

| Slider (distance) | Niveau | alertLevel |
|---|---|---|
| 380 cm | 0 % | 2 (critique) |
| 330 cm | ~14 % | 2 (critique) |
| 300 cm | ~22 % | 1 (attention) |
| 250 cm | ~36 % | 0 (normal) |
| 40 cm | ~94 % | 0 (normal) |

## Marche rapide

### 1. ThingsBoard (eu.thingsboard.cloud)

- Créer deux devices : `CuveGuard-ESP32` et `CuveGuard-Agent`
- Copier le token d'accès de chacun (Device → Manage credentials)
- Copier le device ID de l'ESP32 (Device → Copy device Id)
- Importer `dashboard/cuveguard_dashboard.json` (Dashboards → Import)

### 2. Firmware Wokwi

- Créer un projet ESP32 sur wokwi.com, copier `sketch.ino`, `diagram.json`, `libraries.txt`
- Copier `secrets.example.h` en `secrets.h` dans le projet et y mettre le token du device ESP32
- Lancer la simulation ; le Serial Monitor affiche :
  ```
  Wi-Fi..... OK
  MQTT OK
  [TELEMETRIE] dist=250.0cm niveau=36% T=24.0C H=40% pompe=OFF mode=AUTO alerte=0
  ```

### 3. Agent Python

```bash
pip install -r requirements.txt
cp config/thingsboard_config.example.json config/thingsboard_config.json
# éditer config/thingsboard_config.json
python -m agents.orchestrator
```

Sortie attendue :

```
[TB-REST] Authentification OK
[TB-MQTT] Agent connecte
[ORCHESTRATOR] Demarre — seuils 30%/90%, cycle 10s.
[ORCHESTRATOR] niveau=22% pompe=OFF -> START
[TB-REST] RPC setPump(True) envoyee
[ORCHESTRATOR] niveau=36% pompe=ON -> HOLD
```

## Logique de pilotage (hystérésis)

- Niveau **< 30 %** et pompe arrêtée → `START` (RPC `setPump true`)
- Niveau **> 90 %** et pompe en marche → `STOP` (RPC `setPump false`)
- Entre les deux → `HOLD`

La zone morte 30–90 % constitue l'hystérésis : une pompe démarrée à 29 % ne s'arrête qu'à 90 %, ce qui élimine les allumages/extinctions en rafale. **Mode manuel** : le switch `setManualMode` du dashboard passe l'ESP32 en manuel ; l'agent le détecte (`autoMode = false`, décision `HOLD`) et n'envoie jamais de commande pompe tant que le mode manuel est actif.

## RPC supportées par le firmware

| Méthode | Paramètre | Effet |
|---|---|---|
| `setPump` | `{"value": true/false}` | Allume / éteint la pompe |
| `setManualMode` | `{"value": true/false}` | Bascule manuel / auto |
| `getState` | — | Renvoie `pumpOn` et `manualMode` |

## Télémétries (clés imposées)

**ESP32** : `distanceCm`, `waterLevelPct`, `temperature`, `humidity`, `pumpOn`, `manualMode`, `alertLevel` (0 normal / 1 attention / 2 critique).

**Agent** : `agentDecision` (START/STOP/HOLD), `observedLevelPct`, `pumpCommandSent`, `autoMode`.

## Scénarios de la vidéo de démonstration

1. Montage Wokwi en marche, Serial Monitor visible
2. Dashboard ThingsBoard : jauge de niveau, graphes température/humidité et niveau en direct
3. Slider HC-SR04 → 330 cm (< 15 %) : LED rouge + buzzer, `alertLevel = 2`, agent → `START`, pompe ON
4. Slider → 40 cm (> 90 %) : agent → `STOP`, pompe OFF
5. Switch manuel du dashboard + bouton `setPump` : l'agent reste en `HOLD` (`autoMode = false`)
6. Terminal de l'agent visible + widgets `agentDecision` / `observedLevelPct` sur le dashboard
