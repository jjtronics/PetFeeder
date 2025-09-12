# Contributing to PetFeeder

## Installation de l'environnement

### Arduino IDE
1. Installer [Arduino IDE](https://www.arduino.cc/en/software) ou `arduino-cli`.
2. Ajouter le support de la carte ESP8266.
3. Installer les bibliothèques : WiFiManager, ArduinoJson et PubSubClient.
4. Cloner ce dépôt et ouvrir `PetFeeder.ino`.

### PlatformIO
1. Installer [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html) ou l'extension VS Code.
2. Exécuter `pio run` pour compiler et `pio test` pour lancer les tests.
3. La carte cible est `lolin_d1_mini` (voir `platformio.ini`).

## Style de code C++/Arduino
- Utiliser C++ pour Arduino avec un style proche de l'IDE Arduino.
- Indentation de 2 espaces, accolades ouvrantes à la fin de la ligne.
- Noms de classes en `CamelCase` et fonctions en `camelCase`.
- Limiter la longueur des lignes à 100 caractères.

## Conventions de commit
- Suivre la spécification [Conventional Commits](https://www.conventionalcommits.org/).
- Commits à l'impératif, par exemple : `feat: add feed scheduler`.
- Référencer les issues GitHub pertinentes (`#123`).

## Flux Git
1. Créer une branche depuis `main` pour chaque fonctionnalité ou correction.
2. Pousser la branche sur votre fork et ouvrir une Pull Request.
3. Décrire les changements et demander au moins une revue.
4. Une fois approuvée, fusionner via la PR.

## Tests avant PR
1. `pio run` : vérifier que le projet compile.
2. `pio test` : exécuter les tests unitaires s'ils existent.
3. Tester sur matériel réel lorsque c'est possible.

## Liens
- [README](README.md)
- [Code de conduite](CODE_OF_CONDUCT.md)
