# Patch bay MIDI 1.0

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

Le moteur relie des ports MIDI 1.0 entre eux, comme un patch bay matériel. Il s'appuie sur **Windows MIDI Services** (préversion 12) mais reste **exclusivement MIDI 1.0** : seuls les appareils MIDI 1.0 sont listés, les endpoints MIDI 2.0 sont ignorés, et seuls les messages MIDI 1.0 circulent. ASIO ne transporte pas le MIDI : ces ports sont distincts du pilote audio.

## Ports

| Identifiant | Rôle |
|---|---|
| `midi:virtual:<id>` | Paire de ports MIDI 1.0 créée par le moteur, comme avec loopMIDI : les applications (DAW, synthés logiciels) envoient vers « <nom> In » et écoutent « <nom> Out ». |
| `midi:port:<groupe>:<id Windows>` | Un port d'un appareil MIDI 1.0 (interface USB ou pilote MIDI 1.0 existant). Les endpoints MIDI 2.0 natifs (appareils MIDI 2.0, réseau MIDI 2.0, ports virtuels d'autres applications) ne sont pas proposés. |

Windows MIDI Services représente chaque port MIDI 1.0 d'un appareil par un **groupe** (1 à 16) de son endpoint. Une interface à 4 ports donne donc `midi:port:1:…` à `midi:port:4:…`. L'API indique pour chaque port son nom (celui affiché aux applications MIDI 1.0), s'il peut servir de source (`source`, MIDI In) et de destination (`destination`, MIDI Out). Le sens vient des blocs de fonction de l'appareil, ou de ses blocs de terminaux de groupe USB.

Un port virtuel est fait de deux **Basic MIDI 1.0 Loopbacks** de Windows MIDI Services, de vrais ports MIDI 1.0. Le moteur ne crée jamais de port MIDI 2.0 (appareil virtuel UMP) : ces ports font planter certaines applications MIDI 1.0.

- « <nom> In » : les applications y envoient, le moteur ne fait que le lire ;
- « <nom> Out » : le moteur y écrit, les applications le lisent.

Un loopback renvoie à ses lecteurs tout ce qu'on y écrit. Avec un seul sens par loopback, le moteur ne reçoit jamais ses propres messages. Le nom est limité à 24 caractères ASCII et l'identifiant à 24 caractères, car ils forment les identifiants uniques `tx-<id>-in` et `tx-<id>-out` (32 caractères au plus). Ces identifiants stables permettent aux applications de retrouver les ports après un redémarrage du moteur. Les loopbacks sont temporaires : ils disparaissent quand le service MIDI redémarre, et le moteur réutilise ceux qu'il a laissés s'il a été arrêté brutalement. La protection anti-boucle de Windows reste active : si une application renvoie « Out » vers « In », Windows coupe le loopback.

Le moteur n'ouvre un endpoint que si une route active utilise l'un de ses ports. Une seule connexion porte tous les ports de l'endpoint. Les autres applications peuvent l'utiliser en même temps : Windows MIDI Services est multiclient.

## Routes

Une route relie un port source à un port destination. Plusieurs routes peuvent partir d'une même source (duplication) ou arriver sur une même destination (fusion). Options, toutes facultatives :

- `channels` : canaux source acceptés (1 à 16, tous par défaut) ;
- `messages` : catégories acceptées : `notes`, `polyPressure`, `controlChange`, `programChange`, `channelPressure`, `pitchBend`, `systemRealtime` (horloge, start/stop), `systemCommon`, `sysex` ;
- `outputChannel` : réécrit le canal ;
- `transpose` : décalage en demi-tons. Une note qui sortirait de 0 à 127 est ignorée, à l'enclenchement comme au relâchement, pour qu'aucune note ne reste bloquée ;
- `enabled` : désactive la route sans la supprimer.

Une route reste inactive si sa source ne peut pas émettre, si sa destination ne peut pas recevoir, ou si l'un des ports est absent. Une route d'un port vers lui-même est refusée. Le moteur ne détecte pas les boucles qui passent par une application qui renvoie son entrée vers sa sortie (MIDI thru).

Seuls les messages MIDI 1.0 circulent. Tout autre message (MIDI 2.0, utilitaire, flux UMP, données flexibles) est ignoré ; il n'y a aucune conversion MIDI 2.0.

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

Exécution : Windows 11 avec le runtime Windows MIDI Services préversion 12 (Tools 0.99.94-preview.12, Transports 1.0.43-preview.12). Le transport « Basic MIDI 1.0 Loopback » doit être installé et activé (page Transports de l'outil MIDI Troubleshooting and Repair). Selon les notes de version, ce transport en préversion n'est pas signé et demande le mode développeur de Windows.

La préversion 12 n'apporte pas de nouveau paquet d'API. On compile avec les métadonnées du paquet NuGet `Windows.Devices.Midi2` **0.99.92-preview.11**. La compilation ne télécharge rien. Récupérez le paquet, puis :

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DTIMOX_ENABLE_WINDOWS_MIDI_SERVICES=ON `
  -DTIMOX_MIDI2_WINMD="C:\chemin\vers\Windows.Devices.Midi2.winmd"
```

CMake génère la projection C++/WinRT avec le `cppwinrt.exe` du Windows SDK. Sans cette option, le moteur se compile comme avant et `GET /api/v1/midi` indique `available: false`.

## Limites

- Le backend Windows (`src/windows_midi_services_backend.cpp`) n'a pas encore été compilé ni testé sur une machine Windows équipée de la préversion 12. Les tests automatiques couvrent le routage et la gestion de configuration avec un backend simulé.
- Les noms et le sens des ports viennent de la table de noms MIDI 1.0 et des blocs déclarés par l'appareil. Ce mapping n'a pas encore été vérifié avec de vrais appareils.
- Le format exact des identifiants d'endpoint des loopbacks n'est pas documenté. Le moteur retrouve ses loopbacks orphelins en y cherchant `tx-<id>-in` / `tx-<id>-out` ; à vérifier sous Windows.
- Pas encore d'interface graphique ni d'horloge MIDI synchronisée sur l'horloge audio.
- La supervision (`supervision.adherents[].capabilities.midi`) reste `null` : les ports MIDI ne sont pas encore rattachés aux adhérents audio.
