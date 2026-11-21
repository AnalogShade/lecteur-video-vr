# Prototype WebXR archivé


Le prototype Chrome ou Edge reste disponible via **Lancer Horizon.cmd**. Les fichiers sont lus localement.

### Démarrer le prototype avec le Rift CV1

1. Branche le Rift et ses capteurs. Vérifie que le casque est prêt dans l’application Oculus pour PC (Meta Horizon Link).
2. Lance **Lancer Horizon.cmd** et garde sa fenêtre ouverte. Le lecteur s’ouvre à l’adresse [127.0.0.1:4173](http://127.0.0.1:4173/).
3. Clique sur **Ouvrir des vidéos** ou glisse des fichiers sur le lecteur. Choisis **Écran plat**, **VR 180°** ou **VR 360°**, puis **Mono** ou la disposition 3D de ton fichier.
4. Clique sur **Entrer en VR**, autorise la VR si le navigateur le demande, puis mets ton casque.

Si **Entrer en VR** reste grisé, vérifie dans l’application Oculus pour PC que le runtime OpenXR de Meta est actif (**Paramètres → Général → Runtime OpenXR**), puis ferme et rouvre Chrome ou Edge. [Documentation Meta](https://developers.meta.com/vr/documentation/unity/unity-link/)

### Lecture du prototype

- Dans le casque, vise les commandes en bas avec une manette Touch et utilise la gâchette. Tu peux mettre en pause, accéder à un autre moment, couper le son, passer à la vidéo suivante, recentrer la vue et quitter la VR.
- Pour masquer les commandes, utilise **Masquer** ou serre la poignée. Appuie sur la gâchette pour les faire réapparaître devant toi.
- Sur le PC, les raccourcis sont **Espace** pour lire ou mettre en pause, **←** et **→** pour se déplacer de 10 secondes, **M** pour le son et **R** pour recentrer un panorama.
- Les projections 180° et 360° attendent des vidéos équirectangulaires. La 3D accepte les dispositions côte à côte (SBS) et haut/bas (TB), avec inversion gauche/droite au besoin.
- Les codecs dépendent de Chrome ou Edge. MP4 avec vidéo H.264 et audio AAC est le format à essayer si un fichier est refusé.

### Compiler le prototype

Node.js 22 ou ultérieur. Le lanceur installe les dépendances et compile les fichiers au premier démarrage si la version compilée est absente. Pour recompiler après avoir modifié les sources, utilise `npm run build`.

```powershell
npm ci
npm run build
npm test
npm start
```
