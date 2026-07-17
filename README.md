# CuveGuard — Surveillance d'un réservoir d'eau avec remplissage automatique

Projet IoT Master 1 IA — DIT.

Un ESP32 (Wokwi) mesure le niveau d'une cuve (HC-SR04) et la température / humidité
(DHT22), publie la télémétrie vers ThingsBoard en MQTT, et signale localement un niveau
critique (LED rouge + buzzer). Un **agent Python** lit le niveau sur ThingsBoard et pilote
la pompe (relais + LED) par **RPC** selon les seuils **30 % / 90 %** avec hystérésis. Le
dashboard ThingsBoard permet aussi le **pilotage manuel**.

> Ce README ne reprend pas tout l'énoncé — seulement ce qui n'est pas évident dans notre
> projet (choix d'implémentation, brochage réel, lancement).

## Architecture du dépôt

```
├── README.md
├── requirements.txt              requests, paho-mqtt
├── .gitignore                    wokwi/secrets.h, config/thingsboard_config.json, __pycache__
│
├── agents/                       ── AGENT PYTHON (PC) ────────────
│   ├── orchestrator.py           Boucle principale : point d'entrée (python -m agents.orchestrator)
│   ├── control_agent.py          Décide START / STOP / HOLD (seuils + hystérésis)
│   ├── tb_rest.py                REST → lit la télémétrie ESP32 + envoie les RPC (one-way)
│   └── tb_client.py              MQTT → publie la télémétrie de l'agent
│
├── config/
│   └── thingsboard_config.example.json   host, tokens, seuils 30/90, intervalle
│       (→ copier en thingsboard_config.json, gitignoré)
│
├── wokwi/                        ── FIRMWARE ESP32 ───────────────
│   ├── sketch.ino                HC-SR04 + DHT22 + pompe + alerte, MQTT + RPC
│   ├── diagram.json              Câblage du simulateur
│   ├── libraries.txt             DHT sensor library, Adafruit Unified Sensor, PubSubClient, ArduinoJson
│   └── secrets.h                 Token TB (gitignoré) — voir note « Sécurité » plus bas
│
└── dashboard/
    └── cuveguard.json            Dashboard à importer dans ThingsBoard
```

## Choix d'implémentation (le point important)

**La pompe n'est pilotée QUE par RPC `setPump`.** Le firmware ne fait **aucun**
auto-contrôle local : toute la logique automatique (seuils 30 % / 90 %) est portée par
l'agent Python, conformément à l'énoncé. Deux origines de commande :

- **Agent Python** (mode auto) : applique les seuils et envoie `setPump`.
- **Dashboard** (mode manuel) : les switches envoient `setPump` / `setManualMode`.

Ainsi l'état réel `pumpOn` publié en télémétrie reflète toujours la dernière commande
reçue, sans que le firmware ne « reprenne la main » dans le dos du dashboard ou de l'agent.

En **mode manuel** (`manualMode = true`), l'agent détecte l'état, passe `autoMode = false`,
décide `HOLD` et **n'envoie jamais** de commande pompe tant que le mode manuel est actif.

## Brochage (ESP32 DevKit v1)

| GPIO | Composant |
|---|---|
| 5  | HC-SR04 TRIG |
| 18 | HC-SR04 ECHO |
| 4  | DHT22 DATA |
| 26 | Relais IN (pompe) |
| 27 | LED verte (état pompe, suit le relais) |
| 25 | LED rouge (alerte niveau bas) |
| 33 | Buzzer (niveau critique) |

## Calcul du niveau

Le HC-SR04 est fixé au sommet de la cuve et mesure la distance jusqu'à la surface :

```
niveau (%) = (DIST_VIDE − distance) / (DIST_VIDE − DIST_PLEINE) × 100   (borné 0–100)
```

avec `DIST_VIDE = 400 cm` (0 %) et `DIST_PLEINE = 20 cm` (100 %). En simulation, cliquer
sur le HC-SR04 pendant l'exécution et déplacer le **slider de distance** pour faire varier
le niveau.

| Slider (distance) | Niveau | alertLevel | Effet pompe (auto) |
|---|---|---|---|
| 400 cm | 0 %   | 2 critique  | START (< 30 %) |
| 360 cm | ~11 % | 2 critique  | START |
| 340 cm | ~16 % | 1 attention | START |
| 280 cm | ~32 % | 0 normal    | HOLD |
| 50 cm  | ~92 % | 0 normal    | STOP (> 90 %) |

`alertLevel` : `< 15 %` → **2** (critique, LED rouge + buzzer) ; `15–30 %` → **1**
(attention) ; `≥ 30 %` → **0** (normal).

## Marche rapide

### 1. ThingsBoard (eu.thingsboard.cloud)

- Créer **deux** devices : un pour l'ESP32, un pour l'agent Python.
- Copier le **token d'accès** de chacun (Device → *Manage credentials*).
- Copier le **device ID** de l'ESP32 (Device → *Copy device Id*).
- Importer `dashboard/cuveguard.json` (Dashboards → *Import dashboard*).

### 2. Firmware Wokwi

- Créer un projet ESP32 sur wokwi.com, y copier `sketch.ino`, `diagram.json` et le
  contenu de `libraries.txt`.
- Renseigner le **token du device ESP32** dans `sketch.ino` (constante `TB_TOKEN`).
- Lancer la simulation ; le Serial Monitor (115200 bauds) affiche :
  ```
  Connexion Wi-Fi... OK
  Tentative de connexion MQTT... OK !
  Télémesure transmise : {"distanceCm":280.0,"waterLevelPct":31.5,"temperature":24,"humidity":40,"pumpOn":false,"manualMode":false,"alertLevel":0}
  ```

> **Cadence :** la télémétrie est publiée toutes les **3 s**. Publier plus vite fait
> dépasser la limite de débit du tier gratuit et provoque la **perte des RPC entrants** —
> ne pas descendre en dessous.

### 3. Agent Python

```bash
pip install -r requirements.txt
cp config/thingsboard_config.example.json config/thingsboard_config.json
# éditer config/thingsboard_config.json : tb_username, tb_password,
# esp32_device_id, agent_access_token
python -m agents.orchestrator
```

Sortie attendue :

```
[TB-REST] Authentification OK
[TB-MQTT] Agent connecte
[ORCHESTRATOR] Demarre — seuils 30%/90%, cycle 3s. Ctrl+C pour arreter.
[ORCHESTRATOR] niveau=16% pompe=OFF -> START
[TB-REST] RPC setPump(True) envoyee
[ORCHESTRATOR] niveau=32% pompe=ON -> HOLD
```

Champs de `config/thingsboard_config.json` : `tb_host`, `tb_mqtt_port`, `tb_username`,
`tb_password`, `esp32_device_id`, `agent_access_token`, `seuil_start` (30), `seuil_stop`
(90), `intervalle_s` (3).

## Logique de pilotage (hystérésis)

- Niveau **< 30 %** et pompe arrêtée → `START` (RPC `setPump true`).
- Niveau **> 90 %** et pompe en marche → `STOP` (RPC `setPump false`).
- Entre les deux → `HOLD`.

La zone morte 30–90 % constitue l'hystérésis : une pompe démarrée à 29 % ne s'arrête qu'à
90 %, ce qui élimine les allumages/extinctions en rafale.

**Fiabilité RPC :** le RPC one-way n'étant pas garanti (une commande peut se perdre lors
d'une micro-coupure MQTT), l'agent **réémet** la commande tant que la télémétrie n'a pas
confirmé le nouvel état (réémission au bout de ~6 s), sans jamais spammer.

## RPC supportées par le firmware

| Méthode | Paramètre | Effet |
|---|---|---|
| `setPump` | `{"value": true/false}` | Allume / éteint la pompe |
| `setManualMode` | `{"value": true/false}` | Bascule manuel / auto |
| `getState` | — | Renvoie `pumpOn` et `manualMode` |

## Télémétries (clés imposées)

**ESP32** : `distanceCm`, `waterLevelPct`, `temperature`, `humidity`, `pumpOn`,
`manualMode`, `alertLevel` (0 normal / 1 attention / 2 critique).

**Agent** : `agentDecision` (START/STOP/HOLD), `observedLevelPct`, `pumpCommandSent`,
`autoMode`.

## Dashboard

Widgets fournis : jauge `waterLevelPct`, courbes température/humidité et niveau, cartes
`pumpOn` / `alertLevel` / `agentDecision` / `observedLevelPct`, et **deux switches RPC**
(pompe et mode manuel). Les switches sont configurés en **abonnement télémétrie temps
réel** (`retrieveValueMethod: timeseries`) : leur position suit l'état réel du device
(y compris quand c'est l'agent qui agit), en plus d'envoyer la commande au clic.

## Scénarios de la vidéo de démonstration

1. Montage Wokwi en marche, Serial Monitor visible (télémétrie transmise toutes les 3 s).
2. Dashboard ThingsBoard : jauge de niveau, graphes température/humidité et niveau en direct.
3. Slider HC-SR04 vers **~360 cm** (< 15 %) : LED rouge + buzzer, `alertLevel = 2`,
   agent → `START`, pompe ON.
4. Slider vers **~50 cm** (> 90 %) : agent → `STOP`, pompe OFF.
5. Switch **Mode manuel** du dashboard + switch **Pompe** : l'agent reste en `HOLD`
   (`autoMode = false`) et la pompe suit la commande manuelle.
6. Terminal de l'agent visible + widgets `agentDecision` / `observedLevelPct` sur le dashboard.

## Sécurité (avant de zipper)

L'énoncé impose de **ne pas livrer de tokens ni mots de passe réels**. Avant de créer le
zip :

- Remplacer le token réel dans `sketch.ino` (`TB_TOKEN`) par un placeholder, p. ex.
  `"VOTRE_TOKEN_ESP32"`.
- Vérifier que `config/thingsboard_config.json` (gitignoré) **n'est pas** inclus ; ne
  livrer que `config/thingsboard_config.example.json` avec des placeholders.
- Vérifier que `wokwi/secrets.h` (gitignoré) ne contient pas de token réel s'il est présent.
```