# Box art

Drop PNG/JPG scans here (gitignored - not distributed). The launcher loads:

- `<key>_front.png` - front cover (shown in the carousel)
- `<key>_back.png` - back cover (F / X / click cycles front -> back -> cart)
- `<key>_cart.png` - cartridge scan (third face in the flip cycle; drawn
  aspect-fit on a dark card, alpha respected)

Keys: `worldtour`, `vpw64`, `revenge`, `wm2k`, `vpw2`, `nomercy`.

Any resolution works (decoded via WIC, JPG content in a .png name is fine).
Missing files get a styled placeholder. Fronts AND backs were auto-fetched from
the LaunchBox Games Database (gamesdb.launchbox-app.com) - NA region for the
western releases, Japan for the two VPWs. Replace with better scans anytime.
