# Lecteur vidéo VR

Lecteur vidéo VR lit tes médias locaux sur le Rift CV1. La version Windows utilise le runtime OpenXR d’Oculus, le moteur vidéo libmpv et Direct3D 11. Elle fonctionne sans DeoVR ni navigateur.

## Version Windows native

1. Vérifie que le Rift et ses capteurs sont prêts dans l’application Oculus pour PC.
2. Ouvre le raccourci **Lecteur vidéo VR** sur ton bureau ou lance **native/bin/LecteurVideoVR.exe** depuis le dossier du projet.
3. Mets le casque. Le lecteur démarre la session VR automatiquement et affiche la bibliothèque.
4. Sélectionne un disque ou un favori dans la colonne de gauche. Les dossiers et les vidéos s’affichent à droite. Pointe un élément et appuie une fois sur la **gâchette** ou sur **A** pour l’ouvrir.

Si le Rift est en train de démarrer, le lecteur réessaie pendant 30 secondes. En cas d’échec, la fenêtre Windows affiche le message du runtime Oculus. Le bouton **Entrer en VR** permet alors de réessayer.

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
| Joystick dans la bibliothèque | Haut ou bas parcourt la colonne. Gauche ou droite change de colonne. |
| **A** ou **X** pendant la lecture | Bascule entre lecture et pause. |

La bibliothèque VR affiche les disques montés par Windows. Dans un dossier, **Ajouter ce dossier aux favoris** enregistre son emplacement et l’affiche dans la colonne de gauche. Le même bouton permet de le retirer. Les favoris sont conservés après la fermeture du lecteur.

Les boutons **Liste** et **Grille** changent l’affichage de la bibliothèque dans la fenêtre Windows et dans le casque. Fais défiler la grille avec la molette ou la barre de défilement sous Windows, et avec le joystick dans le casque. Les cartes apparaissent tout de suite et les miniatures se chargent une à la fois pour les vidéos visibles.

Les **Commandes vidéo** donnent accès à la barre de progression, au volume et aux vidéos précédente et suivante du dossier. Dans **Réglages de lecture**, tu peux réappliquer le mode automatique, changer la projection et la stéréo, inverser les yeux, régler le champ ou la convergence 3D et quitter le lecteur.

Le bouton **Quitter**, marqué d’une porte ouverte, ferme le lecteur. Il se trouve en bas à droite de la fenêtre Windows, de la bibliothèque VR et des commandes de lecture, y compris la barre flottante.

Le lecteur enregistre la position des vidéos sur ce PC. Quand tu rouvres une vidéo, elle reprend au dernier point enregistré. Une vidéo terminée repart du début.

### Projections et fichiers

- **Auto** lit les métadonnées spatiales et le nom du fichier. Si la disposition reste inconnue, il compare plusieurs images pendant la lecture pour laisser passer un générique ou un écran noir. Deux vues côte à côte ou haut/bas peuvent ainsi être reconnues même sans balise. [Spécification des métadonnées spatiales](https://github.com/google/spatial-media/blob/master/docs/spherical-video-v2-rfc.md)
- Dans le casque, appuie sur **B** pendant la lecture et ouvre **Format vidéo**. Choisis **180° 3D**, **180° fisheye 3D** ou **360° mono** en une action. Les listes **Projection** et **Disposition** permettent aussi de régler ces paramètres séparément.
- **VR 180°** et **VR 360°** utilisent une projection équirectangulaire. **Fisheye** attend une image circulaire centrée par œil et son champ est réglable. **Cube 3 × 2** accepte l’atlas standard sans bordure. **Écran plat** sert aux vidéos et aux photos ordinaires.
- **Mono**, **Côte à côte (SBS)** et **Haut / bas (TB)** sont disponibles dans chaque projection. Sélectionne **Inverser les yeux** si la profondeur paraît à l’envers.
- Le moteur ouvre notamment les conteneurs MP4, MKV, WebM, MOV et AVI, ainsi que des photos JPEG et PNG. Le codec, le profil d’encodage et la projection varient d’un fichier à l’autre. Les fichiers protégés et les projections propres à certaines caméras nécessitent une prise en charge distincte.
- Sur le PC, glisse dans l’aperçu pour orienter la vidéo. Maintiens **Maj** pendant un glissement horizontal pour régler l’inclinaison. Utilise les boutons **−** et **+** pour le zoom.
- **Lentilles calibrées** utilise le maillage intégré de la caméra pour rendre chaque œil. Pour une vidéo fisheye sans calibration intégrée, le champ de vision reste ajustable dans les réglages. La convergence reste réglable pour les modes stéréoscopiques compatibles. [Format VR180 de Google](https://github.com/google/spatial-media/blob/master/docs/vr180.md)

### Compiler la version native

Windows x64, Visual Studio Build Tools avec le composant C++ Desktop et le SDK Windows.

```powershell
.\native\build.ps1
```

Le script récupère les versions épinglées des dépendances, vérifie leur SHA-256 et place l’exécutable et les DLL ensemble dans **native/bin**. Il remplace l’exécutable au même emplacement après une compilation réussie. Le raccourci du bureau continue de pointer au bon endroit tant que le dossier du projet reste en place. [libmpv](https://github.com/mpv-player/mpv/releases/tag/v0.42.0) · [OpenXR Loader](https://github.com/KhronosGroup/OpenXR-SDK/releases/tag/release-1.1.65)
