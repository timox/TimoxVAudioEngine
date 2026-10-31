# Patch bay MIDI

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

Le moteur relie des ports MIDI entre eux, comme un patch bay matériel. Il s'appuie sur **Windows MIDI Services** (préversion 12). ASIO ne transporte pas le MIDI : les applications voient des ports MIDI Windows ordinaires, distincts du pilote audio.

## Ports

| Identifiant | Rôle |
|---|---|
| `midi:virtual:<id>` | Port créé par le moteur. Les applications (DAW, synthés logiciels) l'ouvrent comme une entrée et une sortie MIDI. Ce qu'elles envoient entre dans le patch bay ; ce que le patch bay y envoie leur parvient. |
| `midi:endpoint:<id Windows>` | Endpoint Windows MIDI Services : interface USB, réseau, Bluetooth ou port d'une autre application. |

Un port virtuel est un appareil virtuel Windows MIDI Services appartenant au moteur. Il existe tant que le moteur tourne. Son identifiant stable (`ProductInstanceId` = `TimoxVasio-<id>`) permet aux applications de le retrouver après un redémarrage. Les applications MIDI 1.0 (WinMM, WinRT MIDI 1.0) le voient sous le nom du port suivi de « MIDI ». Le nom est donc limité à 24 caractères ASCII.

Le moteur n'ouvre un endpoint que si une route active l'utilise. Les autres applications peuvent l'utiliser en même temps : Windows MIDI Services est multiclient.

## Routes

Une route relie une source à une destination. Plusieurs routes peuvent partir d'une même source (duplication) ou arriver sur une même destination (fusion). Options, toutes facultatives :

- `groups`, `channels` : groupes et canaux source acceptés (1 à 16, tous par défaut) ;
- `messages` : catégories acceptées : `notes`, `polyPressure`, `controlChange`, `programChange`, `channelPressure`, `pitchBend`, `systemRealtime` (horloge, start/stop), `systemCommon`, `sysex`, `flexData` ;
- `outputGroup`, `outputChannel` : réécrit le groupe ou le canal ;
- `transpose` : décalage en demi-tons. Une note qui sortirait de 0 à 127 est ignorée, à l'enclenchement comme au relâchement, pour qu'aucune note ne reste bloquée ;
- `enabled` : désactive la route sans la supprimer.

Les messages utilitaires (horodatages de réduction de gigue) et les messages de flux UMP (découverte d'endpoint) appartiennent à une connexion. Ils ne sont jamais routés. Une route d'un port vers lui-même est refusée. Le moteur ne détecte pas les boucles qui passent par une application qui renvoie son entrée vers sa sortie (MIDI thru).

Le routage s'exécute sur les threads de réception de Windows MIDI Services, jamais dans le callback audio. Il travaille en paquets UMP, sans allocation, et le graphe est remplacé d'un bloc à chaque changement de configuration.

## API

- `GET /api/v1/midi` relit la liste des endpoints et rouvre les endpoints routés qui sont revenus (appareil rebranché). Il renvoie ensuite les ports, la configuration et, pour chaque route, `active`, `forwarded` et `filtered`.
- `PUT /api/v1/midi/configuration` remplace la configuration complète, l'applique et l'enregistre dans `%LOCALAPPDATA%\TimoxVasio\midi-patchbay.json`.

```json
{
  "virtualPorts": [{"id": "daw", "name": "Timox DAW"}],
  "routes": [
    {"id": "clavier-vers-daw", "source": "midi:endpoint:\\\\?\\swd#midisrv#...", "destination": "midi:virtual:daw"},
    {"id": "daw-vers-synth", "source": "midi:virtual:daw", "destination": "midi:endpoint:\\\\?\\swd#midisrv#...",
     "channels": [1], "outputChannel": 10}
  ]
}
```

Les ports virtuels inchangés restent ouverts pour les applications ; un port renommé est recréé. Si un port virtuel ne peut pas être créé, l'ancienne configuration reste active. Si un endpoint ne peut pas être ouvert, seules ses routes restent inactives et l'erreur apparaît dans `ports[].error`.

Codes d'erreur : `INVALID_MIDI_CONFIGURATION` (400), `MIDI_CONFIGURATION_FAILED` (500), `MIDI_UNAVAILABLE` (503). Le moteur démarre normalement sans Windows MIDI Services ; seul le patch bay MIDI est alors indisponible.

## Prérequis et compilation

Exécution : Windows 11 avec le runtime Windows MIDI Services préversion 12 (Tools 0.99.94-preview.12, Transports 1.0.43-preview.12). Le transport d'appareils virtuels doit être disponible.

La préversion 12 n'apporte pas de nouveau paquet d'API. On compile avec les métadonnées du paquet NuGet `Windows.Devices.Midi2` **0.99.92-preview.11**. La compilation ne télécharge rien. Récupérez le paquet, puis :

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DTIMOX_ENABLE_WINDOWS_MIDI_SERVICES=ON `
  -DTIMOX_MIDI2_WINMD="C:\chemin\vers\Windows.Devices.Midi2.winmd"
```

CMake génère la projection C++/WinRT avec le `cppwinrt.exe` du Windows SDK. Sans cette option, le moteur se compile comme avant et `GET /api/v1/midi` indique `available: false`.

## Limites

- Le backend Windows (`src/windows_midi_services_backend.cpp`) n'a pas encore été compilé ni testé sur une machine Windows équipée de la préversion 12. Les tests automatiques couvrent le routage et la gestion de configuration avec un backend simulé.
- La préversion signale un problème connu sur la build Windows 11 26100.8875 : les endpoints virtuels peuvent devenir inutilisables (microsoft/MIDI#1047).
- Pas encore d'interface graphique ni d'horloge MIDI synchronisée sur l'horloge audio.
- La supervision (`supervision.adherents[].capabilities.midi`) reste `null` : les ports MIDI ne sont pas encore rattachés aux adhérents audio.
