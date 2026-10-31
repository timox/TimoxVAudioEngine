# Supervision API / API de supervision — schemaVersion 1

## Autorité / authority

Le moteur possède le registre, les liens et les observations. L'adhérent possède ses nœuds. Un adhérent peut demander une connexion et consulter les liens qui le concernent ; leur établissement, maintenance et retrait restent exclusivement au moteur. Cette projection ne décide aucune admission ou exclusion automatique.

The engine owns the registry, links and observations. An adherent owns its nodes. Adherents can request and inspect connections; only the engine establishes, maintains and removes links. This projection implements no automatic admission or removal policy.

GET /api/v1/state ajoute `supervision.schemaVersion=1` sans retirer les champs API 1.1. La GUI conserve ses lectures historiques compatibles pendant sa migration ; la présence de cette projection ne signifie pas que tous ses lecteurs utilisent déjà ces nouveaux chemins.

GET /api/v1/state adds `supervision.schemaVersion=1`, preserving legacy API 1.1 fields. The GUI still uses compatibility surfaces during migration; this does not claim every GUI reader has switched to the new paths.

| Propriété visible / visible property | Autorité et projection / ownership and projection |
|---|---|
| État, erreur, horloge, fréquence et quantum moteur | `supervision.engine` : état moteur effectif / effective engine state |
| Fréquences et tailles de buffers connues du pilote physique | Adhérent device, `capabilities`; inventaire complet dans `inventory.physicalDrivers` / driver support, not measured processing capacity |
| Processus, PID, nom, instance de création | Adhérent process, `identity`; Windows FILETIME chaîne décimale / decimal string |
| Accès ASIO d'un processus, canaux alloués | Nœud asioClient ; ports sources=outputs, sinks=inputs ; `effectiveInfo` fréquence/quantum partagés / shared effective format, not sustainable capacity |
| Accès pilote ASIO physique | Adhérent device à portée driverAccess, hardwareIdentity=null ; nœud asioDevice / driver access does not prove a unique physical card |
| Endpoints Windows et applications détectées | `inventory.wasapiDevices/windowsAudioApplications`, snapshots mis en cache / cached inventory, not participation |
| Configuration WASAPI, activation demandée, rôle par défaut | `engine.configuration.configuredWasapiBridges` ; configuration répétée sur nœud identifié pour navigation / unresolved default role remains intent without a fabricated node |
| Copies WASAPI d'un même processus | Un nœud par bridge, même adhérent seulement PID+creationTime connu identiques / one bridge per node, proven process instance merge only |
| Format négocié, état effectif WASAPI | Nœud `effectiveInfo/state`; compteurs, erreurs, ratio, retries dans `engine.observations.wasapiBridges` avec nodeId / effective negotiated state versus central observations |
| Audio Windows natif | Adhérent device driverAccess native:windows, matériel inconnu ; ports stéréo uniquement attached+running+graphFormat / native driver access, no invented hardware or process identity |
| Activation native demandée | `engine.configuration.nativeAudioEnabled`; état et santé dans `engine.observations.nativeAudio` / requested enabled versus observed state |
| Routes, gain, mute, cible configurée et cible résolue | `links`, owner=engine, requested/applied séparés ; nodeId null si résolution impossible / requested intent is not an applied connection |
| Peak, LUFS, underruns et overruns par canal | `engine.observations.meters`, endpointId/nodeId/adherentId ; aucune mesure copiée sur adhérent/nœud / engine channel measurements, referenced attribution |
| Compteurs callbacks/échanges, âge du dernier callback | `engine.observations.clients[].activity` ; activité ne prouve pas signal sonore / activity is not audible signal |
| CPU processus | `engine.observations.clients` ciblant adhérent ; pourcentage Windows tous processeurs, null si inconnu / observation, never declared sustainable capacity |
| Santé buffers moteur et durées instrumentées | `engine.observations.bufferHealth/performanceMeasurements` ; durées clients ciblées par driverId+processId du producteur historique / do not assign engine-wide deadlines to one process |
| PWAR supervisé : disponibilité, PID, réseau, files, erreurs | `engine.observations.pwarClient`, affirmation du superviseur sur PID courant ; pas de fusion identité par nom / supervisor runtime claim, never infer PWAR by process-name regex |
| Profils applications, paramètres PWAR, diagnostics | `engine.resources` référence les endpoints explicites existants / explicit resource APIs, no repeated disk/inventory scan per telemetry tick |
| Paires stéréo et corrélation sélectionnée | engine.channelPairs (et compatibilité state.stereoPairs) ; événement audio.correlation ; propriétés dérivées des canaux moteur et sélection de mesure / existing engine-derived channel pairs and correlation selection |
| Port d'application MIDI (une appli) | Adhérent `application` (`midi:application:<id>`), identité `midiApplicationPort`, sans PID : le loopback ne révèle pas quel processus l'ouvre ; nœud `midiApplicationPort`, sources=« <nom> In », sinks=« <nom> Out » selon `input`/`output` / engine-owned loopback pair, no process identity claimed |
| Appareil MIDI 1.0 | Adhérent device `device:midi:<endpointId>`, identité `midiEndpoint`, matériel inconnu ; nœud `midiDevice`, un port par groupe MIDI 1.0 ; état open/available/offline / endpoint access, one port per MIDI 1.0 group |
| Routes MIDI | `links` avec `medium=midi`, id `midi:<routeId>` ; requested=route configurée, applied seulement si la route est active / configured intent versus active route |
| Horloge MIDI du moteur et base de temps carte son | Adhérent `engine` (`engine:midi-clock`), nœud `midiEngineClock` ; `engine.observations.midi.timebase` (fréquence mesurée, dérive ppm) et `engineClock` (tempo, marche, ticks) / engine-generated clock counted in card frames, card drift measured against Windows |
| Trafic MIDI par port et par lien | `engine.observations.midi` : reçus/envoyés, dernière activité, transmis/filtrés ; aussi dans `supervision.observations` / engine-observed counters since engine start, never reset by reconfiguration |
| Géométrie GUI, zoom, sélection, dialogs, lissage | Présentation GUI seulement / presentation only |
| Animation « signal flowing » sur câble | Estimation GUI depuis pic source et gain configuré ; aucune mesure indépendante de lien n'est revendiquée / display estimate, not independently measured link signal |

## Disponibilité et temps / availability and time

Les observations WASAPI conservent `bridgeId`, `state`, santé et retries même si un rôle par défaut ne résout aucun endpoint. `nodeId` et `adherentId` sont alors null : une erreur reste observable sans inventer de participant. Un endpoint explicitement configuré reste une identité d'accès connue lorsque `effectiveInfo` ne fournit pas encore d'endpoint ; cela ne prouve pas un flux actif.

WASAPI observations retain `bridgeId`, `state`, health and retries even when a default role resolves no endpoint. `nodeId` and `adherentId` are then null: failures remain observable without inventing participants. An explicitly configured endpoint remains a known access identity when `effectiveInfo` does not yet supply an endpoint; it does not prove an active stream.

`readAtUnixMs` date la lecture API, pas l'acquisition audio. Les niveaux runtime sont la dernière valeur atomique sans timestamp d'acquisition. `availability=observed` ne garantit donc ni âge maximal ni nouvel échantillon depuis la précédente réponse. Un arrêt/reprise peut republier une valeur ancienne ; une séquence d'acquisition runtime serait nécessaire pour éliminer cette ambiguïté.

`readAtUnixMs` is API read time, not audio acquisition time. Runtime levels are the last atomic value without an acquisition timestamp. `availability=observed` therefore promises neither bounded age nor a new sample. Stop/restart can replay a previous value; an actual runtime acquisition sequence would be required to remove that ambiguity.

Pour une source ASIO (output du client vers moteur), stopped/detached/initialized/buffers-ready, statut illisible, ou successfulExchangeCount connu nul rendent peak/LUFS null et availability=unavailable avec raison. Un sink ASIO (input du client) mesure la production du moteur : l'arrêt du client ne suffit pas à invalider cette mesure. Un endpoint virtuel dont le client a disparu devient indisponible. Les compteurs restent visibles ; null n'est jamais converti en silence ou zéro.

For an ASIO source (client output), stopped/detached/initialized/buffers-ready, unreadable status, or known zero successful exchanges make peak/LUFS null with an unavailable reason. An ASIO sink measures engine production: client stop alone does not invalidate that level. A virtual endpoint whose client disappeared is unavailable. Counters remain visible; missing values are never converted to silence or zero.

## Événements / events

`devices.changed` compare la topologie en excluant uniquement activity et CPU dynamiques. Un changement d'identité/ports déclenche toujours l'événement. `supervision.observations` émet environ toutes les 500 ms (y compris pendant les commandes), avec owner=engine, readAtUnixMs et clients. Les niveaux restent dans `audio.meter` à la même cadence. La GUI ne doit pas effacer des niveaux uniquement parce qu'un compteur d'activité évolue.

`devices.changed` compares topology excluding only dynamic activity and CPU. Identity/port changes still trigger it. `supervision.observations` publishes approximately every 500 ms, including during commands, with owner=engine, readAtUnixMs and clients. Levels remain in `audio.meter` at the same cadence. GUI clients must not clear levels merely because an activity counter changes.

## Limites explicites / explicit limits

Capacité de traitement soutenable inconnue : null. `capabilities.midi` reste null pour les adhérents audio ; les adhérents MIDI sont MIDI 1.0 uniquement. Identité runtime PID sans creationTime n'est pas une identité durable. Les endpoints WASAPI restent des identités d'accès ; aucun regroupement matériel n'est inféré à partir du containerId. Les bindings natifs ne fournissent pas de PID propriétaire. Les inventaires Windows sont les caches déjà acquis ; cette projection ne relance aucune énumération OS. Les propriétés globales de supervision ne constituent pas un résultat de mesure indépendant pour chaque lien.

Sustainable processing capacity remains unknown/null. `capabilities.midi` stays null for audio adherents; MIDI adherents are MIDI 1.0 only. Runtime PID identity without creationTime is not durable. WASAPI endpoints remain access identities; hardware grouping is not inferred from containerId. Native bindings expose no owner PID. Windows inventories use existing caches without OS re-enumeration. Global observations do not imply an independent measurement for every link.

Le cache processus vide sans preuve de rafraîchissement produit windowsAudioApplications=null et windowsAudioApplicationsState=unknown ; une erreur explicite donne unavailable. Un cache non vide est marqué cached et peut être ancien. / Empty process cache without refresh evidence is unknown/null; an explicit error is unavailable, and a nonempty cache is marked cached, possibly stale.
