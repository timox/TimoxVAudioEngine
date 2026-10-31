# Patch bay MIDI 1.0

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

Le moteur relie des ports MIDI 1.0 entre eux, comme un patch bay matériel. Il s'appuie sur **Windows MIDI Services** (préversion 12) mais n'utilise que des **ports MIDI 1.0** : ceux que voient les applications MIDI 1.0 (WinMM, WinRT MIDI 1.0). ASIO ne transporte pas le MIDI : ces ports sont distincts du pilote audio.

## Ports

| Identifiant | Rôle |
|---|---|
| `midi:virtual:<id>` | Port créé par le moteur : une entrée et une sortie MIDI 1.0 que les applications (DAW, synthés logiciels) ouvrent. Ce qu'elles envoient entre dans le patch bay ; ce que le patch bay y envoie leur parvient. |
| `midi:port:<groupe>:<id Windows>` | Un port MIDI 1.0 d'un endpoint : interface USB, réseau, Bluetooth ou port d'une autre application. |

Windows MIDI Services représente chaque port MIDI 1.0 d'un appareil par un **groupe** (1 à 16) de son endpoint. Une interface à 4 ports donne donc `midi:port:1:…` à `midi:port:4:…`. L'API indique pour chaque port son nom (celui affiché aux applications MIDI 1.0), s'il peut servir de source (`source`, MIDI In) et de destination (`destination`, MIDI Out). Le sens vient des blocs de fonction de l'appareil, ou de ses blocs de terminaux de groupe USB.

Un port virtuel est un appareil virtuel Windows MIDI Services appartenant au moteur et déclaré **MIDI 1.0 uniquement** : un seul bloc bidirectionnel sur le groupe 1. Les applications MIDI 1.0 le voient sous le nom du port suivi de « MIDI ». Le nom est donc limité à 24 caractères ASCII. Le port existe tant que le moteur tourne. Son identifiant stable (`ProductInstanceId` = `TimoxVasio-<id>`) permet aux applications de le retrouver après un redémarrage.

Le moteur n'ouvre un endpoint que si une route active utilise l'un de ses ports. Une seule connexion porte tous les ports de l'endpoint. Les autres applications peuvent l'utiliser en même temps : Windows MIDI Services est multiclient.

## Routes

Une route relie un port source à un port destination. Plusieurs routes peuvent partir d'une même source (duplication) ou arriver sur une même destination (fusion). Options, toutes facultatives :

- `channels` : canaux source acceptés (1 à 16, tous par défaut) ;
- `messages` : catégories acceptées : `notes`, `polyPressure`, `controlChange`, `programChange`, `channelPressure`, `pitchBend`, `systemRealtime` (horloge, start/stop), `systemCommon`, `sysex` ;
- `outputChannel` : réécrit le canal ;
- `transpose` : décalage en demi-tons. Une note qui sortirait de 0 à 127 est ignorée, à l'enclenchement comme au relâchement, pour qu'aucune note ne reste bloquée ;
- `enabled` : désactive la route sans la supprimer.

Une route reste inactive si sa source ne peut pas émettre, si sa destination ne peut pas recevoir, ou si l'un des ports est absent. Une route d'un port vers lui-même est refusée. Le moteur ne détecte pas les boucles qui passent par une application qui renvoie son entrée vers sa sortie (MIDI thru).

Seuls les messages MIDI 1.0 circulent. Si un appareil MIDI 2.0 envoie des messages MIDI 2.0, le moteur les convertit en MIDI 1.0 selon la traduction par défaut de la norme UMP : valeurs réduites, banque convertie en CC 0/32 puis changement de programme, RPN/NRPN convertis en CC 101/100 ou 99/98 puis 6/38. Les messages sans équivalent MIDI 1.0 (contrôleurs par note, contrôleurs relatifs) sont ignorés, comme les messages utilitaires, de flux UMP et de données flexibles.

Le routage s'exécute sur les threads de réception de Windows MIDI Services, jamais dans le callback audio. Il ne fait aucune allocation, et le graphe est remplacé d'un bloc à chaque changement de configuration.

## API

- `GET /api/v1/midi` relit la liste des ports MIDI 1.0 et rouvre les ports routés qui sont revenus (appareil rebranché). Il renvoie ensuite les ports, la configuration et, pour chaque route, `active`, `forwarded` et `filtered`.
- `PUT /api/v1/midi/configuration` remplace la configuration complète, l'applique et l'enregistre dans `%LOCALAPPDATA%\TimoxVasio\midi-patchbay.json`.

```json
{
  "virtualPorts": [{"id": "daw", "name": "Timox DAW"}],
  "routes": [
    {"id": "clavier-vers-daw", "source": "midi:port:1:\\\\?\\swd#midisrv#...", "destination": "midi:virtual:daw"},
    {"id": "daw-vers-synth", "source": "midi:virtual:daw", "destination": "midi:port:2:\\\\?\\swd#midisrv#...",
     "channels": [1], "outputChannel": 10}
  ]
}
```

Les ports virtuels inchangés restent ouverts pour les applications ; un port renommé est recréé. Si un port virtuel ne peut pas être créé, l'ancienne configuration reste active. Si un endpoint ne peut pas être ouvert, seules les routes de ses ports restent inactives et l'erreur apparaît dans `ports[].error`.

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
- Les noms et le sens des ports viennent de la table de noms MIDI 1.0 et des blocs déclarés par l'appareil. Ce mapping n'a pas encore été vérifié avec de vrais appareils.
- Les ports virtuels basiques MIDI 1.0 (« Basic MIDI 1.0 Loopback ») de la préversion 12 ne sont pas utilisés : un même endpoint renvoie aux lecteurs tout ce qu'on y écrit, le moteur recevrait donc ses propres messages. Leur création demande aussi le mode développeur dans cette préversion.
- Pas encore d'interface graphique ni d'horloge MIDI synchronisée sur l'horloge audio.
- La supervision (`supervision.adherents[].capabilities.midi`) reste `null` : les ports MIDI ne sont pas encore rattachés aux adhérents audio.
