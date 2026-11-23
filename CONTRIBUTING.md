# Contribuer à JSG Guitar

Merci de votre intérêt ! Corrections de bugs, nouvelles pédales, traductions, idées : tout est bienvenu.

## Signaler un problème

Ouvrez une **Issue** en indiquant :
- votre système (Windows 10/11, macOS…) et votre carte son ;
- ce que vous avez fait, ce qui s'est passé, ce que vous attendiez ;
- le journal si le son ne démarre pas : `%LOCALAPPDATA%\JSG Guitar\journal.txt` (Windows) ou
  `~/Library/Application Support/JSG Guitar/journal.txt` (Mac).

## Proposer une modification

1. Faites un *fork* du dépôt et créez une branche.
2. Gardez le style du code existant (commentaires en français, C++20).
3. Vérifiez que la compilation automatique (onglet Actions) passe.
4. Ouvrez une *Pull Request* qui explique ce que fait la modification.

Chaque Pull Request d'une personne extérieure est relue par le responsable du projet avant d'être intégrée
(voir [CODE_SIGNING.md](CODE_SIGNING.md)).

## Licence des contributions

En proposant une contribution, vous acceptez qu'elle soit publiée sous la licence du projet :
**GPL v3 ou ultérieure** pour le code, **CC BY-SA 4.0** pour les images. Vous restez l'auteur de votre
contribution et vous êtes crédité dans l'historique du dépôt.

N'ajoutez que votre propre travail, ou celui d'un tiers sous licence compatible avec la GPL v3.
En particulier, **aucun code de JUCE** et aucun composant propriétaire.

Ne déposez pas les fichiers MP3 des playbacks dans le dépôt : ils sont distribués à part.
