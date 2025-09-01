# PetFeeder

[Français](#français) | [English](#english)

---

## English

### Overview
PetFeeder is an open‑source connected cat feeder based on an ESP8266 (Wemos D1 mini) and a NEMA17 stepper motor driven by an A4988 driver. It offers a modern web interface, daily schedule, OTA updates and optional MQTT/webhook integrations.

*Insert a photo of the assembled feeder here*
![Assembled PetFeeder](docs/images/feeder.jpg)

### Features
- Self‑hosted Wi‑Fi configuration portal (WiFiManager)
- Responsive Web UI with schedule management and manual actions
- Daily quota tracking with 7‑day history chart
- Expert mode toggle to reveal advanced settings
- OTA firmware updates with configurable password
- Local JSON API endpoints
- Physical button for feed/unclog/safe‑mode toggle
- Optional MQTT and webhook notifications

### Hardware
- Wemos D1 mini (ESP8266)
- A4988 stepper driver
- NEMA17 stepper motor
- 12 V power supply + 100–220 µF capacitor
- Push button between D2 and GND
- 3D printed parts based on [YouTube design](https://www.youtube.com/watch?v=Uv0lsih8JRA) and [Ko‑fi model](https://ko-fi.com/s/698e04b7e3)

### Wiring
| Wemos D1 mini | A4988 | Notes |
|---------------|-------|-------|
| D5 (GPIO14)   | DIR   | direction |
| D6 (GPIO12)   | STEP  | step pulse |
| D7 (GPIO13)   | EN    | LOW = enabled |
| 3V3           | VDD   | logic power |
| 3V3           | SLEEP & RESET | bridge together |
| 12V           | VMOT  | motor power |
| GND           | GND   | common ground |

Motor coils (NEMA17 17HE12‑1204S example):
- Coil A → A4988 1A/1B: black / blue
- Coil B → A4988 2A/2B: green / red

*Insert a wiring diagram or photo here*
![Wiring diagram](docs/images/wiring.png)

### Software Setup
1. Install [Arduino IDE](https://www.arduino.cc/en/software) or `arduino-cli` with ESP8266 board package.
2. Open `PetFeeder_code_V3.1.ino` and select **ESP8266 › LOLIN(Wemos) D1 R2 & mini**.
3. Flash the board. On first boot the access point `<device_name>-SETUP` will appear.
4. Connect and configure Wi‑Fi credentials, timezone, quotas, etc.

*Insert a screenshot of the web interface here*
![Web UI](docs/images/web-ui.png)

### API & MQTT
- `GET /status` – JSON state
- `GET /feed?n=1` – dispense `n` rations
- `GET /unclog` – reverse/forward to clear jams
- `/api/schedule` – GET/POST schedule
- MQTT topic prefix: `<cfg.mqtt_topic>` (e.g. `petfeeder/cmd/feed`)

### Credits
Hardware design inspired by the linked video/model. Electronics and firmware rewritten for ESP8266 connectivity.

### License
This project is released under the MIT License. See [LICENSE](LICENSE).

---

## Français

### Aperçu
PetFeeder est un distributeur de croquettes connecté basé sur un ESP8266 (Wemos D1 mini) et un moteur pas‑à‑pas NEMA17 piloté par un A4988. Il propose une interface web moderne, un planning journalier, des mises à jour OTA et des intégrations MQTT/webhook facultatives.

*Insérer ici une photo du distributeur assemblé*
![Distributeur assemblé](docs/images/feeder.jpg)

### Fonctionnalités
- Portail de configuration Wi‑Fi autonome (WiFiManager)
- Interface Web responsive pour gérer le planning et les actions manuelles
- Suivi du quota journalier avec graphique sur 7 jours
- Mode Expert pour afficher les paramètres avancés
- Mises à jour OTA avec mot de passe configurable
- API JSON locale
- Bouton physique pour nourrir/débourrer/activer le mode sécurisé
- Notifications MQTT et webhook en option

### Matériel
- Wemos D1 mini (ESP8266)
- Driver A4988
- Moteur pas‑à‑pas NEMA17
- Alimentation 12 V + condensateur 100–220 µF
- Bouton poussoir entre D2 et GND
- Pièces imprimées en 3D basées sur le [design YouTube](https://www.youtube.com/watch?v=Uv0lsih8JRA) et le [modèle Ko‑fi](https://ko-fi.com/s/698e04b7e3)

### Câblage
| Wemos D1 mini | A4988 | Notes |
|---------------|-------|-------|
| D5 (GPIO14)   | DIR   | sens de rotation |
| D6 (GPIO12)   | STEP  | impulsion de pas |
| D7 (GPIO13)   | EN    | LOW = activé |
| 3V3           | VDD   | alimentation logique |
| 3V3           | SLEEP & RESET | ponter ensemble |
| 12V           | VMOT  | puissance moteur |
| GND           | GND   | masse commune |

Bobines moteur (ex. NEMA17 17HE12‑1204S) :
- Bobine A → A4988 1A/1B : noir / bleu
- Bobine B → A4988 2A/2B : vert / rouge

*Insérer ici un schéma de câblage ou une photo*
![Schéma de câblage](docs/images/wiring.png)

### Mise en route
1. Installer l'[IDE Arduino](https://www.arduino.cc/en/software) ou `arduino-cli` avec la carte ESP8266.
2. Ouvrir `PetFeeder_code_V3.1.ino` et choisir **ESP8266 › LOLIN(Wemos) D1 R2 & mini**.
3. Téléverser le firmware. Au premier démarrage, le point d'accès `<device_name>-SETUP` apparaît.
4. S'y connecter pour configurer le Wi‑Fi, le fuseau horaire, les quotas, etc.

*Insérer ici une capture d’écran de l’interface*
![Interface web](docs/images/web-ui.png)

### API & MQTT
- `GET /status` – état en JSON
- `GET /feed?n=1` – distribue `n` rations
- `GET /unclog` – débloque la vis sans fin
- `/api/schedule` – GET/POST du planning
- Préfixe de topic MQTT : `<cfg.mqtt_topic>` (ex. `petfeeder/cmd/feed`)

### Remerciements
Design mécanique inspiré de la vidéo/modèle ci-dessus. Électronique et firmware réécrits pour l’ESP8266.

### Licence
Ce projet est publié sous licence MIT. Voir [LICENSE](LICENSE).

