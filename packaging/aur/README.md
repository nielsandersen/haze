# AUR packaging

`PKGBUILD` and `.SRCINFO` for `hyprhaze`, the Arch package of Haze. The
package is named `hyprhaze` because `haze` is taken; both install
`/usr/bin/haze`, so they conflict.

Build and install it locally:

```bash
cd packaging/aur
makepkg -si
```

It downloads the tagged release archive from GitHub, builds it, runs the
tests, and installs under `/usr`.

For a new release, set `pkgver`, update `sha256sums` (`updpkgsums`, from
`pacman-contrib`), and regenerate the metadata:

```bash
makepkg --printsrcinfo > .SRCINFO
```
