# Lecteur vidéo VR

Lecteur vidéo VR est un petit projet personnel, sans prétention, qui propose une alternative aux lecteurs vidéo VR pour regarder les fichiers que tu as déjà sur ton PC. Je l’ai créé pour pouvoir enfiler mon Rift CV1, retrouver un dossier et reprendre une vidéo là où elle s’était arrêtée.

Le lecteur fonctionne directement sous Windows, sans DeoVR, navigateur ni compte propre au lecteur. Tes médias, tes favoris et tes positions de lecture restent sur ton PC. La lecture passe par libmpv. Pour afficher l’image et utiliser les manettes dans le casque, le lecteur s’appuie sur OpenXR et Direct3D 11. Avec un Rift, le runtime Meta sur le PC et un pilote graphique compatible restent nécessaires. [OpenXR pour PC chez Meta](https://developers.meta.com/vr/documentation/native/pc/dg-openxr/)

## Casques et prérequis

| Casque | État de la compatibilité |
| --- | --- |
| Oculus Rift CV1 | Casque de référence utilisé pour développer le lecteur. |
| Oculus Rift S | Compatibilité attendue via le runtime OpenXR de Meta. À valider sur le casque. |
| Meta Quest 2, 3, 3S et Pro | Compatibilité à valider en PC VR via Meta Horizon Link par câble ou Air Link en Wi-Fi. Le lecteur s’exécute sur le PC Windows. |

Le lecteur cible Windows x64 et les commandes Oculus Touch. Avec un casque Meta, vérifie que **Meta Horizon Link** est actif comme runtime OpenXR sur le PC. Pour un Quest, lance aussi la session Link depuis le casque. [Activer le runtime OpenXR de Meta](https://developers.meta.com/vr/documentation/unity/unity-link/) · [Configurer Link et Air Link](https://www.meta.com/help/quest/509273027107091/)

## Premiers pas

1. Depuis les sources, installe **Visual Studio Build Tools** avec le composant **Développement Desktop en C++** et le **SDK Windows**.
2. Ouvre Meta Horizon Link et vérifie que ton casque et tes manettes sont prêts. Sur le Rift CV1, branche aussi les capteurs.
3. Double-clique sur [Lancer Lecteur video VR.cmd](Lancer%20Lecteur%20video%20VR.cmd) à la racine du projet. Au premier lancement, le script télécharge les dépendances et compile le lecteur si nécessaire. Tu peux ensuite lancer `native\bin\LecteurVideoVR.exe` directement en gardant les DLL à côté.
4. Mets le casque. Choisis un disque ou un favori à gauche, puis pointe un dossier ou une vidéo à droite. Appuie sur la **gâchette** ou sur **A** pour l’ouvrir.

Le lecteur essaie de démarrer la session VR pendant 30 secondes. En cas d’échec, la fenêtre Windows affiche le message d’OpenXR. Une fois le casque prêt, clique sur **Entrer en VR** pour réessayer.

## Bibliothèque et lecture

La bibliothèque affiche les disques montés par Windows. Dans un dossier, **Ajouter ce dossier aux favoris** épingle son emplacement à gauche. Le même bouton permet de le retirer. Les favoris sont conservés après la fermeture du lecteur.

Passe de **Liste** à **Grille** pour afficher des aperçus. Les cartes apparaissent tout de suite et les miniatures sont générées en arrière-plan, une vidéo visible à la fois. Fais défiler la grille à la molette ou avec la barre de défilement sous Windows, et au joystick dans le casque.

Le lecteur mémorise la position des vidéos sur ce PC. Une vidéo terminée repart du début. Le bouton **Quitter**, marqué d’une porte ouverte, se trouve en bas à droite de la fenêtre Windows, de la bibliothèque VR et des menus de lecture.

### Manettes Touch

| Commande | Action |
| --- | --- |
| Pointer et appuyer sur la gâchette ou sur **A** dans un menu | Confirme l’élément surligné en une seule action. |
| Appuyer sur la gâchette sous l’index pendant la lecture | Affiche une petite barre de lecture flottante. Appuie à côté de la barre ou utilise **B** pour la fermer. |
| Pointer le curseur de lecture et maintenir la gâchette sous l’index | Fais glisser le curseur, puis relâche pour aller au moment choisi. |
| Pointer le curseur de volume et maintenir la gâchette sous l’index | Fais glisser le curseur pour régler le volume en direct. |
| Pointer **Sourdine** ou **Rétablir le son**, puis appuyer sur la gâchette sous l’index | Coupe ou rétablit le son au volume choisi. |
| Maintenir la gâchette latérale de la poignée, tourner ou incliner la manette, puis relâcher | Oriente la vidéo et conserve l’angle choisi. |
| Joystick gauche ou droite pendant la lecture | Recule ou avance de 10 secondes. Maintenir répète le saut. |
| Gâchette latérale maintenue + joystick gauche ou droite sur la même manette | Ouvre la vidéo précédente ou suivante dans le dossier. Ramène le joystick au centre avant de changer de vidéo à nouveau. |
| Joystick haut ou bas pendant la lecture | Rapproche ou éloigne l’image. |
| Appuyer sur le joystick | Recentre la vue et le menu, et rétablit le zoom. |
| **B** ou **Y** | Revient au dossier parent ou à l’écran précédent. Depuis la vidéo, revient à la bibliothèque. |
| Joystick dans la bibliothèque | En liste, haut et bas parcourent les éléments et gauche ou droite change de colonne. En grille, haut et bas parcourent les rangées et gauche ou droite parcourt les cartes. À gauche de la grille, un mouvement vers la gauche ramène aux disques et favoris. |
| **A** ou **X** pendant la lecture | Bascule entre lecture et pause. |

Les **Commandes vidéo** donnent accès à la barre de progression, au volume et aux vidéos précédente et suivante du dossier. Dans **Réglages de lecture**, tu peux réappliquer le mode automatique, changer la projection et la stéréo, inverser les yeux et régler le champ ou la convergence 3D.

## Projections et fichiers

- **Auto** lit les métadonnées spatiales et le nom du fichier. Si la disposition reste inconnue, il compare plusieurs images pendant la lecture pour laisser passer un générique ou un écran noir. Deux vues côte à côte ou haut/bas peuvent ainsi être reconnues même sans balise. [Spécification des métadonnées spatiales](https://github.com/google/spatial-media/blob/master/docs/spherical-video-v2-rfc.md)
- Dans le casque, appuie sur **B** pendant la lecture et ouvre **Format vidéo**. Choisis **180° 3D**, **180° fisheye 3D** ou **360° mono** en une action. Les listes **Projection** et **Disposition** permettent aussi de régler ces paramètres séparément.
- **VR 180°** et **VR 360°** utilisent une projection équirectangulaire. **Fisheye** attend une image circulaire centrée par œil et son champ est réglable. **Cube 3 × 2** accepte l’atlas standard sans bordure. **Écran plat** sert aux vidéos et aux photos ordinaires.
- **Mono**, **Côte à côte (SBS)** et **Haut / bas (TB)** sont disponibles dans chaque projection. Sélectionne **Inverser les yeux** si la profondeur paraît à l’envers.
- Le moteur ouvre notamment les conteneurs MP4, MKV, WebM, MOV et AVI, ainsi que des photos JPEG et PNG. Le codec, le profil d’encodage et la projection varient d’un fichier à l’autre. Les fichiers protégés et les projections propres à certaines caméras nécessitent une prise en charge distincte.
- Sur le PC, glisse dans l’aperçu pour orienter la vidéo. Maintiens **Maj** pendant un glissement horizontal pour régler l’inclinaison. Utilise les boutons **−** et **+** pour le zoom.
- **Lentilles calibrées** utilise le maillage intégré de la caméra pour rendre chaque œil. Pour une vidéo fisheye sans calibration intégrée, le champ de vision reste ajustable dans les réglages. La convergence reste réglable pour les modes stéréoscopiques compatibles. [Format VR180 de Google](https://github.com/google/spatial-media/blob/master/docs/vr180.md)

## Compiler depuis les sources

Windows x64, Visual Studio Build Tools avec le composant C++ Desktop et le SDK Windows.

```powershell
.\native\build.ps1
```

Le script récupère les versions épinglées des dépendances, vérifie leur SHA-256 et place l’exécutable et les DLL ensemble dans `native/bin`. Il remplace l’exécutable au même emplacement après une compilation réussie. [libmpv](https://github.com/mpv-player/mpv/releases/tag/v0.42.0) · [OpenXR Loader](https://github.com/KhronosGroup/OpenXR-SDK/releases/tag/release-1.1.65)

Le [prototype Web](prototype-web/README.md) est documenté séparément.

## Partager un retour

Si tu essaies le lecteur avec un autre casque ou que tu rencontres un problème, indique le modèle du casque et des manettes, le runtime OpenXR utilisé, le mode de connexion et ce que tu as observé. Pour un problème de lecture, le format de la vidéo et le message d’erreur aident aussi à reproduire le problème.
