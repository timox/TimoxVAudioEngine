# API locale de contrôle TimoxVasio v1

**Français** | [English](API.en.md)

Cette API est le contrat entre l’interface de contrôle et le moteur natif. Electron main en est le client ; le renderer ne lit ni le registre, ni le fichier de configuration, ni la mémoire audio partagée.

Pour une prise en main par étapes, avec schéma d’architecture et exemples
PowerShell/Node.js, voir le [quick start API](API_QUICKSTART.md). Ce fichier
reste la référence détaillée du contrat.

La définition [Swagger/OpenAPI 3.1](../api/openapi-v1.json) décrit les routes HTTP et référence les schémas JSON partagés. Son extension `x-websocket` décrit les commandes, réponses et événements de la connexion WebSocket. Le [guide des fonctions](FONCTIONS.md) illustre les mesures et la console intégrée.

Le serveur natif utilise `cpp-httplib` 0.58.0 pour HTTP et WebSocket, et `nlohmann/json` 3.12.0 pour encoder et décoder les messages. Les en-têtes et licences de ces dépendances sont conservés dans `vendor/`.

L’inventaire physique énumère les pilotes ASIO enregistrés sans les ouvrir. `capabilitiesKnown` vaut `false` et les capacités sont vides tant qu’un pilote n’a pas été explicitement sélectionné et ouvert par le moteur.

Les messages sont encodés en JSON UTF-8. Les schémas normatifs sont dans [`schemas/api-v1.json`](../api/schemas/api-v1.json). Les unités sont explicites dans les champs : fréquence en hertz, taille de buffer en frames, canal numéroté à partir de 1, gain en dB et niveau de crête en dBFS.

## Adresse et lecture

Le moteur écoute uniquement sur l’interface loopback `127.0.0.1`. Le port peut être demandé à son démarrage ; le port `0` demande un port disponible. Le processus client récupère le port sélectionné dans l’information de démarrage du moteur.

Le contrôle depuis Linux n’est pas disponible : aucun accès API distant n’est
ouvert. L’appairage par certificats et la passerelle TLS décrits dans la
[proposition de contrôle distant](superpowers/specs/2026-10-24-remote-timox-api-pairing.md)
ne sont pas implémentés.

| Requête | Réponse |
|---|---|
| `GET /api/v1/state` | État courant confirmé du moteur, sélection matérielle, configuration TimoxVasio, routes, inventaires et paires stéréo actives |
| `GET /api/v1/drivers` | Inventaire des pilotes matériels ASIO découverts, du pilote virtuel TimoxVasio et des paires stéréo mesurables |
| `GET /api/v1/openapi.json` | Document OpenAPI servi localement pour Swagger UI |
| `GET /api/v1/schemas/api-v1.json` | Schéma JSON partagé référencé par le document OpenAPI |
| `GET /api/v1/application-profiles` | Profils de capacité de canaux ASIO appliqués selon le nom de l’exécutable |
| `PUT /api/v1/application-profiles` | Remplacement complet et persistant des profils de capacité |
| `GET /api/v1/pwar` | Adresse réseau PWAR enregistrée dans `pwarASIO.cfg` |
| `PUT /api/v1/pwar` | Enregistre l’adresse et le port de destination PWAR sous Windows |
| `ws://127.0.0.1:<port>/api/v1/ws` | Commandes de configuration et événements d’état/audio |

Ces deux routes gèrent uniquement les paramètres réseau Windows enregistrés.
`PUT` renvoie `reopenRequired` : il faut redémarrer le client PWAR Windows pour
qu'il recharge ces paramètres. Timox ne démarre pas ce client et ces routes ne
signalent ni sa connexion ASIO, ni l'activité réseau ou audio. Quand le client
ouvre `TimoxVasio.dll`, il apparaît dans `virtualDrivers[].clients` comme une
application ASIO ordinaire. Dans le graphe, route une source vers ses entrées
pour envoyer le son vers Linux ; routez ses sorties vers une destination pour
recevoir le son depuis Linux. La carte ASIO physique locale reste sélectionnée
indépendamment.

Les deux routes HTTP sont appelées sous le même préfixe `http://127.0.0.1:<port>`.

Le déroulé API puis redémarrage d'hôte est schématisé dans
[Séquences du pilote](DRIVER_SEQUENCES.md#2-profil-par-application-via-lapi-et-swaggeropenapi).
Le document [OpenAPI](../api/openapi-v1.json) est la définition utilisée pour
présenter ces routes dans Swagger UI.

Les réponses HTTP sont des objets JSON conformes à `definitions.state` ou `definitions.drivers`. Un pilote matériel contient ses ports d’entrée et de sortie, les taux annoncés par son pilote ainsi que ses capacités de buffer. TimoxVasio expose les processus directement attachés via ASIO et leurs canaux alloués par `createBuffers`. Un client reste listé si ses tableaux `inputChannels` et `outputChannels` sont vides : il a ouvert le pilote, mais n’a pas encore créé de ports routables. Cette adhésion directe ne prouve pas à elle seule que des buffers contiennent un signal audible.

Le snapshot expose aussi `nativeAudio` en lecture seule. `enabled` décrit
l'intention du contrôleur ; `state` vaut `disabled`, `starting`, `running`,
`unavailable`, `denied`, `ownedElsewhere`, `removed` ou `failed`. Un état conservé
du worker ne suffit pas à déclarer le backend actif : `running` exige un manager
attaché et un worker en fonctionnement. `masterRunning` et `attached` sont les
observations du manager. Une erreur du worker peut précéder son retrait du
graphe : `attached=true` seul ne prouve donc pas que le flux fonctionne.

`nativeFormat` décrit le protocole fixe PCM32 stéréo à 48 kHz, pas la détection
d'un périphérique installé. `graphFormat` vaut `null` hors fonctionnement ;
sinon il contient `sampleRate`, `channels` et `maxQuantum`. `epoch` et
`wireGeneration` sont des chaînes décimales conservant les 64 bits. `retryInMs`
est un délai en millisecondes ; `systemError` conserve le code Windows brut.
Les compteurs `health` suffixés `NativeFrames` et `discardedPacingFrames`
comptent des frames à 48 kHz ; `captureSilencedGraphFrames` compte des frames
à la fréquence du graphe. Les autres compteurs comptent des événements. Ils
peuvent conserver la dernière observation après désactivation et ne constituent
pas une mesure de latence ni de silence acoustique.

Ce lot n'ajoute aucune commande d'activation native : `nativeAudio` est encore
refusé dans `configuration.apply`. L'authentification API/IPC et la qualification
du service restent requises avant activation publique du backend.

Forme des identifiants d’extrémités :

```text
physical:<driverId>:input:<channel>
physical:<driverId>:output:<channel>
virtual:TimoxVasio:<client-pid>:input:<channel>
virtual:TimoxVasio:<client-pid>:output:<channel>
```

Le suffixe canal est un entier décimal à partir de 1. Les ID renvoyés par le moteur sont à réutiliser tels quels dans la configuration et les vues.

`GET /api/v1/state` distingue `routes`, les routes actuellement actives, de
`configuredRoutes`, toutes les routes enregistrées. Les routes d’une application
absente restent dans `configuredRoutes` avec un identifiant stable de la forme
`virtual:TimoxVasio:app:<executable>:output:<channel>` ou `:input:<channel>`.
Le moteur les réactive lorsque cette application ouvre de nouveau les canaux,
même si son PID a changé. L’API accepte ces identifiants stables déjà présents
dans `configuredRoutes` lors d’une modification de configuration. Un nouvel
identifiant stable peut aussi être soumis pour une application déclarée dans
`application-profiles`, dans la limite de son nombre de canaux annoncé. Si plusieurs
instances portent le même nom d’exécutable, la route attend une correspondance
non ambiguë. Les anciennes routes enregistrées uniquement avec un PID ne
peuvent pas être attribuées à une autre application après disparition du
processus ; elles restent enregistrées pour correction explicite.

## Profils de capacité par application

`GET /api/v1/application-profiles` renvoie la liste complète des profils explicitement configurés :

```json
{
  "profiles": [
    { "processName": "mixxx.exe", "inputChannels": 255, "outputChannels": 255 }
  ]
}
```

Le nom doit être un nom de fichier exécutable Windows, sans chemin. La comparaison ne distingue pas les majuscules. Une application absente de la liste reçoit 256 entrées et 256 sorties. Le profil initial de Mixxx est fixé à 255 dans les deux directions pour éviter que sa version stable interprète la capacité 256 comme zéro. La limite s’applique à `getChannels`, `getChannelInfo` et `createBuffers`; elle ne change pas la capacité maximale de 256 voies par sens. Le transport privé échange uniquement les voies effectivement créées par l’hôte ASIO, indépendamment des routes. Les applications peuvent donc échanger par les canaux communs réellement alloués et les routes de l’API.

`PUT /api/v1/application-profiles` remplace la liste entière :

```json
{
  "profiles": [
    { "processName": "mixxx.exe", "inputChannels": 255, "outputChannels": 255 },
    { "processName": "autre-hote.exe", "inputChannels": 128, "outputChannels": 64 }
  ]
}
```

Les comptes doivent être compris entre 1 et 256 inclusivement. Une liste vide supprime les dérogations et rétablit la capacité par défaut de 256 pour chaque application. La réponse confirme les profils enregistrés et liste dans `restartRequiredClients` les clients déjà connectés dont le nombre annoncé change. Un hôte déjà initialisé conserve sa réponse `getChannels`; il faut le fermer puis le relancer pour prendre son nouveau profil. Les modifications ne redémarrent ni ne déconnectent les applications à la place de l’utilisateur.

Une requête invalide renvoie le statut HTTP 400 et une erreur structurée `INVALID_APPLICATION_PROFILES`. Le stockage est atomique; si l’enregistrement échoue, le profil actif précédent est conservé.

Après l’ouverture explicite d’un pilote physique, le champ `name` de ses endpoints reprend le nom de canal renvoyé par `IASIO::getChannelInfo`. Si le pilote ne fournit pas de nom exploitable, le moteur publie un nom de remplacement composé du nom du pilote, de la direction et du numéro de canal. Avant l’ouverture, les endpoints et capacités physiques sont vides.

## WebSocket

Chaque commande est un message JSON :

```json
{
  "id": "request-42",
  "command": "configuration.apply",
  "payload": {
    "physicalDriverId": "physical-driver-stable-id",
    "sampleRate": 48000,
    "bufferFrames": 256,
    "routes": [
      {
        "id": "route-1",
        "sourceEndpointId": "virtual:TimoxVasio:1234:output:1",
        "destinationEndpointId": "physical:physical-driver-stable-id:output:1",
        "gainDb": 0.0,
        "mute": false
      }
    ]
  }
}
```

`id` est une chaîne non vide de 1 à 128 caractères et doit être unique parmi les commandes en attente sur cette connexion. La commande `configuration.apply` remplace le pilote physique, la fréquence, la taille de buffer et la liste complète des routes. L’objet complet est validé avant mutation. Une répétition ne duplique pas les routes; elle peut néanmoins interrompre brièvement le flux pendant la reconfiguration.

`GET /api/v1/state` expose `configurationRevision`, incrémenté après chaque configuration enregistrée. Un client qui édite une copie de l'état peut envoyer `expectedConfigurationRevision` dans le payload de `configuration.apply`. Si une autre application a appliqué une configuration entre-temps, l'API répond `CONFIGURATION_CONFLICT` avant toute mutation. Les clients qui omettent ce champ conservent le comportement historique de remplacement complet ; ils doivent relire l'état et préserver les routes des autres applications.

`sampleRate` et `bufferFrames` sont les valeurs du pilote physique choisi. `null` demande respectivement le taux courant et la taille préférée annoncés par ce pilote. Le moteur fixe le taux physique, relit ses capacités dépendantes du taux et refuse toute taille qu’il n’annonce pas. TimoxVasio reçoit exactement ces valeurs ; il n’a pas de réglage de fréquence ou de taille indépendant.

`GET /api/v1/state` et `GET /api/v1/drivers` incluent `stereoPairs`. Ce sont
les paires de sorties L/R adjacentes d’un même client, dont les deux canaux sont
ouverts et reliés par une route. `audio.correlation.start` sélectionne une paire
par son `stereoPairId`; `audio.correlation.stop` arrête la mesure. La sélection
est globale au moteur et une seule paire est mesurée à la fois.

Les commandes `{ "id": "start-1", "command": "engine.start" }` et
`{ "id": "stop-1", "command": "engine.stop" }` n’ont pas de payload.

La commande `{ "id": "mesures-1", "command": "performance.measurements.set", "payload": { "enabled": true } }` active les mesures par étape ; `false` les arrête et efface la fenêtre précédente. Elle est idempotente. Les mesures sont désactivées à chaque démarrage du moteur. `GET /api/v1/state` expose toujours `performanceMeasurements` avec `enabled`, `sampleEveryCallbacks: 16` et `window`. La fenêtre vaut `null` avant cinq secondes complètes ou lorsque la collecte est désactivée. Une fenêtre complète porte `durationMs: 5000`, `clients` et `physical`. Chaque client identifié par `processId` et `driverId` expose `hostCallback`, `ringWrite`, `ringWait` et `ringWaitDiscardedBlocks` ; le moteur physique expose `clientRead`, `routing` et `outputCopy`. Chaque durée vaut `null` sans observation, ou contient `samples`, `meanNs`, `worstNs` en nanosecondes. L'attente de l'anneau mesure la publication jusqu'à la lecture d'un bloc entier correspondant ; les lectures partielles sont écartées. La remise au tampon physique ne mesure pas la latence matérielle ou acoustique. Cette collecte n'ajuste ni tampon ni phase.

Chaque client de `virtualDrivers[].clients` dans `/api/v1/state` et `/api/v1/drivers` expose aussi `processCpuPercent` et `processCpuWindowMs`. Le pourcentage est le temps CPU du processus Windows divisé par le temps écoulé et le nombre de processeurs logiques actifs ; il vaut `null` avant le deuxième relevé ou si le processus ne peut pas être interrogé. La fenêtre vaut alors également `null`. La mesure est effectuée lors des requêtes API, hors du callback audio, et ne détaille pas les VST hébergés par l'application.
`engine.start` réapplique la dernière configuration confirmée; la commande est
idempotente et laisse le moteur arrêté si aucune route n’est configurée.
`engine.stop` arrête le flux audio, ferme le pilote matériel et conserve la
configuration afin qu’un prochain `engine.start` puisse la restaurer. Cette
commande est refusée avec `ENGINE_CLIENTS_CONNECTED` tant qu’un client ASIO
TimoxVasio est connecté. Les deux commandes ne ferment pas le processus hôte ni
le serveur HTTP/WebSocket. Un programme externe peut donc contrôler le moteur
tant que cet hôte API a déjà été lancé par Timox VASIO Control ou un
superviseur; l’API ne peut pas démarrer un processus hôte qui n’existe pas.

La commande `{ "id": "reset-1", "command": "engine.reset" }` rouvre le pilote physique avec la dernière configuration enregistrée, même si le moteur tourne déjà. Elle conserve les connexions et interrompt brièvement le son ; elle n'applique pas les modifications en attente dans l'interface.

Un succès est acquitté avec le même identifiant :

```json
{ "id": "request-42", "success": true, "result": { "accepted": true } }
```

Un refus ou un échec renvoie une erreur structurée :

```json
{
  "id": "request-42",
  "success": false,
  "error": {
    "code": "UNSUPPORTED_SAMPLE_RATE",
    "message": "Le pilote matériel ne prend pas en charge 88200 Hz",
    "operation": "open",
    "driverId": "physical-driver-stable-id",
    "asioError": -998
  }
}
```

Les codes d’erreur sont stables et leur texte peut être localisé :

| Code | Signification |
|---|---|
| `INVALID_CONFIGURATION` | Champs manquants ou configuration mal formée |
| `UNKNOWN_ENDPOINT` | Une extrémité n’existe pas dans l’inventaire courant |
| `INVALID_ROUTE_DIRECTION` | Le lien n’est pas l’un des trois types autorisés |
| `UNSUPPORTED_SAMPLE_RATE` | Le taux n’est pas accepté par le pilote ASIO physique choisi |
| `UNSUPPORTED_BUFFER_SIZE` | Le pilote refuse le nombre de frames demandé |
| `PHYSICAL_DRIVER_OPEN_FAILED` | Le pilote matériel ne peut pas être ouvert ou démarré |
| `AUDIO_ENGINE_UNAVAILABLE` | Le moteur ou le transport d’un client VASIO est indisponible |
| `INTERNAL_ERROR` | Erreur non classée pendant l’application de configuration |

## Interruption de configuration

Un changement effectif arrête le flux avant de détruire ou reconstruire les buffers et le graphe. L’ordre observable est :

1. réponse de commande `accepted` ;
2. événement `engine.status` avec `state: "reconfiguring"` ;
3. événement `engine.status` avec `state: "stopped"` si le pilote physique est configuré mais qu’aucune route n’existe encore ; ses capacités et ses endpoints sont alors publiés par `/api/v1/drivers`, sans démarrer de flux audio ;
4. événement `engine.status` avec `state: "running"` si au moins une route existe et que la validation, l’ouverture du périphérique, la création des buffers et le démarrage réussissent ;
5. sinon, événement `engine.error` puis `engine.status` avec `state: "error"`. Le moteur reste arrêté et ne réactive pas l’ancien graphe.

Une configuration identique à celle qui est déjà active peut reconstruire le flux. Une commande invalide est rejetée avant toute interruption. L’interface désactive les mutations pendant `reconfiguring` et affiche l’état confirmé par le moteur.

L’état `stopped` sans route signifie que le pilote a été configuré mais que le callback audio n’a pas démarré. Après `engine.stop`, l’état `stopped` peut aussi contenir des routes mémorisées, prêtes à être restaurées par `engine.start`. Le serveur HTTP/WebSocket reste disponible dans les deux cas pendant la durée de vie du processus moteur; à la fermeture de la fenêtre Electron, le processus moteur reste disponible pour les clients API qui en dépendent.

## Événements

Chaque événement a la forme `{ "event": "<nom>", "payload": { ... } }` et n’a pas de champ `id`.

| Événement | Payload |
|---|---|
| `engine.status` | État complet `state`, `physicalDriverId`, `sampleRate`, `bufferFrames`, `lastError` |
| `devices.changed` | Inventaires `physicalDrivers` et `virtualDrivers` conformes à `/api/v1/drivers` |
| `routes.changed` | Liste complète et confirmée `routes` |
| `audio.meter` | `endpointId`, `peakDbfs`, `shortTermLufs` (null pendant la mesure initiale de 3 s), `lufsWindowSeconds`, compteurs cumulatifs `underruns` et `overruns` |
| `engine.error` | Erreur structurée avec `code`, `message` et, si disponible, opération, pilote et code ASIO |
| `audio.stereoPairs.changed` | `{ "stereoPairs": [...] }`, liste complète des paires L/R ouvertes et routées |
| `audio.correlation` | `stereoPairId`, identifiants L/R, coefficient borné à `[-1, 1]` ou `null`, et `state` (`measuring`, `no_signal` ou `stopped`) |

Les événements de mesure sont émis pour les extrémités présentes dans une route, à la cadence de lecture de l'API (environ deux fois par seconde). `peakDbfs` décrit la crête du bloc audio le plus récent. `shortTermLufs` est la mesure de loudness court terme BS.1770, filtrée K et calculée sur une fenêtre glissante de 3 secondes; elle vaut `null` tant que cette fenêtre n'est pas remplie. `lufsWindowSeconds` indique la durée de cette fenêtre. Les compteurs des endpoints virtuels sont cumulatifs par sens pendant la vie du client, y compris lors du remplacement d'une génération du transport audio; ils sont remis à zéro à la création d'un nouveau contrôle client. Ceux des endpoints physiques valent zéro. Ces événements ne servent pas de transport audio. La mesure est publiée par le serveur hors du callback temps réel.

La corrélation compare les échantillons synchrones des deux sorties du client
sur des fenêtres de 100 ms. Le calcul est un coefficient de corrélation
normalisé à décalage nul, centré sur la moyenne. Il est publié à 10 Hz. Si le
signal RMS de l’un ou l’autre canal est inférieur à −90 dBFS, `correlation` est
`null` et `state` vaut `no_signal`; zéro signifie alors une faible corrélation,
pas un silence. Le coefficient décrit une cohérence large bande et n’est pas
un angle de phase en degrés. Les échantillons restent dans le callback natif :
aucun PCM n’est transmis à l’API.

## Règles de routage

Une route est canal par canal et contient `id`, `sourceEndpointId`, `destinationEndpointId`, `gainDb` entre -120 et +24 et `mute`. Seuls ces liens sont acceptés :

1. sortie TimoxVasio vers entrée TimoxVasio ;
2. sortie TimoxVasio vers sortie ASIO physique ;
3. entrée ASIO physique vers entrée TimoxVasio.

Les identifiants doivent exister et leurs directions doivent correspondre. Les taux ou formats incompatibles sont refusés ; aucune conversion implicite de fréquence n’est effectuée. TimoxVasio annonce une capacité maximale de 256 entrées et de 256 sorties. L’API publie uniquement les canaux réellement alloués par chaque client, tandis que les endpoints physiques reflètent les capacités découvertes après application du taux.

## Sécurité locale et frontière client

Le moteur ne se lie pas aux interfaces réseau externes. Il accepte les connexions WebSocket locales sans en-tête `Origin` depuis le client natif et l’origine applicative VASIO déclarée ; les autres origines sont rejetées. La GUI obtient les inventaires, routes, erreurs et diagnostics uniquement par cette API. Le format des structures de mémoire audio et leur gestion restent privés au moteur et aux DLL.

## Diagnostic et cycle de vie

`GET /api/v1/diagnostics?limit=N` retourne le niveau de journalisation (`info` ou `debug`) et les entrées les plus récentes. `limit` vaut 200 par défaut et accepte 1 à 500. Chaque entrée contient `timestamp`, `level`, `component` et `message`.

`PUT /api/v1/diagnostics` accepte exactement `{ "level": "info" }` ou `{ "level": "debug" }` et persiste ce choix. Les journaux sont écrits sous `%LOCALAPPDATA%\TimoxVasio\logs\engine.log`; quatre archives sont conservées. Les callbacks audio n’écrivent jamais dans le journal.

`engine.start` et `engine.stop` arrêtent ou redémarrent le flux audio sans fermer le processus hôte ni son API. `engine.stop` est refusée avec `ENGINE_CLIENTS_CONNECTED` tant qu’un client TimoxVasio est attaché. À la fermeture de la fenêtre Electron, le processus moteur reste disponible pour les clients API qui en dépendent.

## Source de cadence du moteur

`engine.clockSource` vaut `{ "kind": "asio" }` ou `{ "kind": "software" }`. `engine.clockCapabilities` décrit les fréquences et tailles de bloc disponibles, ou vaut `null` sans horloge configurée. L'inventaire `physicalDrivers` reste réservé aux pilotes ASIO physiques.

`configuration.apply.payload.clockSource` est optionnel : son absence conserve la source courante dans la transaction contrôlée par `expectedConfigurationRevision`. Une ancienne configuration disque sans ce champ conserve le mode ASIO. Les champs existants restent requis. En mode logiciel, `physicalDriverId` doit valoir `null`, `sampleRate` doit être un entier entre 8000 et 384000 et `bufferFrames` entre 16 et 8192. Le moteur peut alors cadencer le graphe sans périphérique physique. Les connexions vers du matériel non sélectionné restent enregistrées mais inactives.

`engine.start`, `engine.stop` et `engine.reset` s'appliquent également à la cadence logicielle. Une panne du worker d'horloge arrête l'audio et publie un état d'erreur ; la reprise est explicite. Les événements `engine.status` exposent la même source et les mêmes capacités que l'état HTTP.

## Ponts WASAPI — contrat 1.1

Les chemins `/api/v1` et le sous-protocole WebSocket `vasio.api.v1` restent inchangés. L'état annonce `apiVersion: "1.1"`. `GET /api/v1/wasapi-devices` rafraîchit l'inventaire des endpoints ; l'état courant utilise un inventaire en cache. Chaque endpoint peut aussi publier son `containerId` Windows (facultatif), qui permet de regrouper les entrées et sorties du même périphérique matériel.

Les moteurs qui prennent en charge la copie audio par processus exposent `capabilities.wasapiProcessLoopback: true` dans `/api/v1/state`. Ce champ facultatif permet aux clients de distinguer cette capacité des autres fonctions WASAPI ; son absence signifie qu'elle n'est pas annoncée par le moteur connecté.

`GET /api/v1/windows-audio-applications` actualise explicitement les sessions audio Windows actives associées à un seul processus. La réponse contient le PID, l'heure de création, le chemin de l'exécutable, le nom affiché et les sorties Windows utilisées, ainsi que `windowsAudioApplicationInventoryError`. Si ce champ n'est pas `null`, l'énumération a échoué : un tableau vide ne signifie alors pas qu'aucune session n'existe. Cela atteste une lecture par le mixeur audio partagé de Windows, pas l'API appelée par l'application. Les clients ASIO Timox restent visibles séparément dans l'inventaire des pilotes virtuels ; un processus peut apparaître dans les deux.

Dans le graphe, un client ASIO Timox est une adhésion directe : l'application a ouvert `TimoxVasio.dll` et lui remet ses buffers. Un pont `processLoopback` est une copie audio créée par Timox : il ajoute l'audio partagé du processus comme source distincte. Cette copie ne raccorde pas l'application au pilote, ne redirige pas sa lecture et ne prouve pas quel pilote audio elle a choisi. Les deux sources restent distinctes même si elles correspondent au même processus.

Chaque client ASIO expose aussi `activity` : état du pilote, nombre de callbacks reçus depuis le dernier démarrage, échanges de blocs complets, frames échangées et âge du dernier callback. `available` indique si le statut partagé du pilote a pu être lu ; `stable` précise si les compteurs forment un instantané cohérent. Ces compteurs prouvent qu'un callback ou un échange de bloc a eu lieu, pas qu'il contenait un signal audible. Dans `bufferHealth`, `callbackCount: 0` avec un état `insufficient` signifie qu'aucun callback du pilote ASIO physique maître n'est arrivé pendant cette fenêtre : les clients ASIO ne peuvent alors pas recevoir de blocs cadencés, même si l'état général du moteur est `running`.

`configuration.apply.payload.wasapiBridges` contient la configuration souhaitée : `id`, `kind` (`capture`, `render`, `endpointLoopback`, `processLoopback`), exactement un sélecteur `endpointId` ou `defaultRole` (`console`, `multimedia`, `communications`) pour les périphériques, et les options `enabled` et `bufferDuration100ns`. `processLoopback` demande plutôt `processId`, `processCreationTime` sous forme de chaîne décimale et `processImagePath`. Il est uniquement en capture et l’identité du processus est vérifiée avant et après l’activation asynchrone. Le moteur garde un handle vers le processus vérifié et refuse de démarrer ou de lire un paquet de capture si ce processus s’est terminé ; un nouveau processus ayant réutilisé le PID ne remplace pas ce handle. Le pont crée dans le graphe une source indépendante à partir d’une copie du flux audio partagé du processus cible **et de ses processus enfants** ; la lecture d’origine continue séparément. La capture Windows couvre leurs sorties audio partagées, pas uniquement un périphérique. Raccorder la copie à la même sortie que l’original peut produire un doublon. Les flux ASIO et WASAPI exclusifs ne sont pas capturés. Omettre le tableau conserve les ponts existants ; `[]` les supprime. Les routes associées doivent être retirées explicitement lors d'une suppression.

L'état distingue `configuredWasapiBridges` de `wasapiBridges` : ce dernier expose le format effectif, la génération, la reprise et les compteurs `health` (pertes, silence, dérive et erreurs). Les ports du graphe utilisent `wasapi:<id>:input:<canal>` ou `wasapi:<id>:output:<canal>`, avec des canaux à partir de 1. La configuration d'un pont précède la découverte de ses ports.

`health.controlBacklogUpdates` compte les observations de régulation forcées après 100 ms de traitement inachevé des données en attente. Cumulatif pour la génération courante, ce diagnostic signale une mesure sur une portion du traitement ; il ne compte pas les pertes audio. Son absence sur un ancien moteur API 1.1 signifie « non disponible », pas zéro. `correctionPpm` inclut la réaction au remplissage et ne mesure pas directement la dérive de l'horloge matérielle.

Cette étape prend en charge le mode partagé, le loopback d'endpoint et le loopback par processus du son partagé. Elle ne fournit pas de pilote Windows virtuel natif et ne capture pas les flux exclusifs ou ASIO. Voir le [guide ASIO/WASAPI](ASIO_WASAPI_GUIDE.md) et le schéma JSON pour les champs et limites exacts.
