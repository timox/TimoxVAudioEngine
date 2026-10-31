# Patch bay MIDI 1.0

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

Le moteur relie des ports MIDI 1.0 entre eux, comme un patch bay matériel. Il s'appuie sur **Windows MIDI Services** (préversion 12) mais reste **exclusivement MIDI 1.0** : seuls les appareils MIDI 1.0 sont listés, les endpoints MIDI 2.0 sont ignorés, et seuls les messages MIDI 1.0 circulent. ASIO ne transporte pas le MIDI : ces ports sont distincts du pilote audio.

## Ports

| Identifiant | Rôle |
|---|---|
| `midi:virtual:<id>` | Port d'application créé par le moteur, un par application : « <nom> In » (l'appli envoie au patch bay) et « <nom> Out » (le patch bay envoie à l'appli), chacun activable séparément. |
| `midi:port:<groupe>:<id Windows>` | Un port d'un appareil MIDI 1.0 (interface USB ou pilote MIDI 1.0 existant). Les endpoints MIDI 2.0 natifs (appareils MIDI 2.0, réseau MIDI 2.0, ports virtuels d'autres applications) ne sont pas proposés. |

Windows MIDI Services représente chaque port MIDI 1.0 d'un appareil par un **groupe** (1 à 16) de son endpoint. Une interface à 4 ports donne donc `midi:port:1:…` à `midi:port:4:…`. L'API indique pour chaque port son nom (celui affiché aux applications MIDI 1.0), s'il peut servir de source (`source`, MIDI In) et de destination (`destination`, MIDI Out). Le sens vient des blocs de fonction de l'appareil, ou de ses blocs de terminaux de groupe USB.

On dédie un port d'application à chaque application. Chaque sens est un **Basic MIDI 1.0 Loopback** distinct de Windows MIDI Services, un vrai port MIDI 1.0. Le moteur ne crée jamais de port MIDI 2.0 (appareil virtuel UMP) : ces ports font planter certaines applications MIDI 1.0.

- « <nom> In » (`input`, activé par défaut) : l'application y envoie, le moteur ne fait que le lire. Le port peut alors servir de source.
- « <nom> Out » (`output`, activé par défaut) : le moteur y écrit, l'application le lit. Le port peut alors servir de destination.

Au moins un des deux doit être activé. Activer ou désactiver un sens ne touche pas l'autre : l'application garde le port resté ouvert. Les routes qui utilisent un sens désactivé restent configurées mais inactives.

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

## Observation et topologie

Les applications gardent leurs propres ports MIDI, hors du patch bay : Windows ne dit pas qui parle à qui. En dédiant un loopback à chaque port d'application, tout le trafic de l'application passe par un point que le moteur lit. Le moteur peut ainsi reconstituer une topologie observable :

- chaque port d'application devient un adhérent `application` avec un nœud `midiApplicationPort` ; chaque appareil MIDI 1.0 devient un adhérent `device` avec un nœud `midiDevice` ;
- chaque route devient un lien `medium=midi` entre deux nœuds, « appliqué » seulement quand la route est active ;
- le moteur compte, par port, les messages reçus et envoyés avec l'heure du dernier message, et par route les messages transmis et filtrés. Un message qui arrive sans route est quand même compté.

### Activité et horloge

Une horloge MIDI envoie 24 ticks (F8) par noire, soit 48 messages par seconde à 120 BPM : compter tous les messages ferait croire qu'un port joue en permanence. Le moteur sépare donc, pour chaque port :

- `activity` : tous les messages MIDI 1.0 sauf les ticks d'horloge et l'active sensing (FE). Un témoin d'activité doit se baser sur `activity.lastUnixMs` ;
- `clock` : nombre de ticks, horloge en marche (un tick il y a moins d'une seconde), tempo mesuré (`bpm`, moyenne glissante sur les intervalles entre ticks), dernier transport reçu (start, continue, stop) ;
- `activeSensing` : messages de présence envoyés par certains appareils.

**Émetteur d'une horloge.** Une horloge MIDI 1.0 ne contient aucune adresse d'expéditeur : seul le port d'arrivée l'identifie. Comme chaque application a son propre loopback « In », le port sur lequel arrivent les ticks désigne l'application (ou l'appareil) qui les émet. `engine.observations.midi.clockSources` liste ces ports avec leur tempo.

Un cas trompeur reste : le moteur transmet l'horloge d'un port A vers une application B, et B la renvoie sur son « In » (MIDI thru). B ressemble alors à une source. Quand le moteur envoie lui-même de l'horloge à un port qui en émet, il marque ce port `echoSuspected` : c'est probablement un écho, pas une vraie source.

Cette topologie apparaît dans `supervision` (`GET /api/v1/state`), et les compteurs dans `engine.observations.midi` et l'événement WebSocket `supervision.observations` (environ toutes les 500 ms). Voir [l'API de supervision](SUPERVISION_API.md). Limite : un loopback ne révèle pas quel processus l'ouvre. L'adhérent est le port d'application configuré, pas un processus prouvé.

## Horloge de référence et dérive

L'**horloge de référence** est l'horloge qui cadence le graphe audio (`engine.clockSource`) : le pilote ASIO physique, ou l'horloge logicielle du moteur sans carte. Elle sert de base de temps à tout le MIDI du moteur. Il ne faut pas la confondre avec les rôles maître/esclave d'une horloge MIDI : ceux-ci disent qui envoie les ticks, alors que l'horloge de référence dit sur quelle échelle de temps ils sont placés.

Une carte a sa propre fréquence réelle, légèrement différente de sa valeur nominale (48 000 Hz) et des temporisateurs de Windows : quelques dizaines de ppm, soit environ 0,1 s par heure. Un MIDI cadencé par les temporisateurs Windows dériverait donc par rapport à l'audio. Le moteur ne s'y rabat jamais.

- **Mesure.** À chaque buffer, l'horloge de référence publie sa position d'échantillons :
  - en ASIO, la position fournie par le pilote (`samplePosition`), et non un compte de callbacks qui manquerait les buffers perdus ;
  - avec l'horloge logicielle, l'échéancier du bloc.

  Chaque position est associée à l'instant du compteur de performance Windows (QPC). Une régression linéaire sur environ 30 s donne la fréquence réelle de l'horloge de référence (`referenceClock.measuredSampleRate`) et son écart en ppm. Le compteur sert uniquement à convertir un instant en échantillons en horodatage d'envoi. La mesure est disponible après environ 2 s. `referenceClock.source` vaut `asio`, `software`, ou `none` quand elle ne tourne pas.
- **Horloge du moteur** (`midi:clock:engine`).
  - Le moteur génère une horloge MIDI en comptant les échantillons de l'horloge de référence. Chaque tick (24 par noire) est placé à son instant en échantillons, converti en horodatage avec la fréquence mesurée, puis envoyé à l'avance. Windows MIDI Services l'émet à l'heure dite : cette horloge ne dérive jamais de l'audio.
  - On la route comme n'importe quelle source.
  - `PUT /api/v1/midi/clock` règle le tempo (`bpm`, enregistré) et le transport (`start`, `stop`, `continue`, envoyés aux destinations).
  - `start` et `continue` exigent une horloge de référence en marche (`409 NO_REFERENCE_CLOCK`).
  - Si l'horloge de référence s'arrête, l'horloge du moteur attend (`waitingForReferenceClock`) sans envoyer de tick, puis repart dès qu'elle revient.
- **Horloge entrante recalée** (`reclock: true` sur une route).
  - Les ticks reçus d'une appli ou d'un appareil ne sont plus transmis à leur arrivée : le patch bay les réémet sur une grille régulière en échantillons de l'horloge de référence.
  - Une régression sur les 48 derniers ticks (deux temps) prédit la place de chaque tick ; il est envoyé à cet instant plus 2 ms de marge.
  - Il y a exactement un tick en sortie par tick en entrée : le tempo et la position des temps de la source sont conservés, seule la gigue (USB, Windows) disparaît.
  - Start, stop et continue passent tels quels et remettent la grille à zéro.
  - Sans horloge de référence en marche, les ticks sont transmis tels quels.
- **Mesure du tempo.** Le tempo affiché pour un port est mesuré en échantillons de l'horloge de référence, par régression sur les 48 derniers ticks : c'est le tempo que l'enregistrement audio montrera.

Une horloge entrante dont l'oscillateur est indépendant de l'horloge de référence (une boîte à rythmes avec son propre quartz) garde son propre tempo : le recalage supprime sa gigue, pas l'écart de fréquence entre les deux oscillateurs. L'horloge du moteur, elle, est construite sur l'horloge de référence et n'a pas cet écart.

## API

- `GET /api/v1/midi` relit la liste des ports MIDI 1.0 et rouvre les ports routés qui sont revenus (appareil rebranché). Il renvoie ensuite les ports, la configuration et, pour chaque route, `active`, `forwarded` et `filtered`.
- `PUT /api/v1/midi/configuration` remplace la configuration complète, l'applique et l'enregistre dans `%LOCALAPPDATA%\TimoxVasio\midi-patchbay.json`.

```json
{
  "virtualPorts": [{"id": "daw", "name": "Timox DAW"}, {"id": "seq", "name": "Sequenceur", "output": false}],
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
