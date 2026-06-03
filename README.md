# [Simon Tatham's Puzzle Collection][p] as a Progressive Web App (PWA)

This project makes the puzzle collection available as a [PWA][] that you
can install on in device via the PWA's [website][]. You can also just play
the puzzles directly on the site, enjoying full persistence like an app.

## Build

You will need Emscripten. Then:

    $ ecmake cmake -B build -DPUZZLES_SDL3=ON -DCMAKE_INSTALL_PREFIX=/var/www
    $ cmake --build build -t install

Alternatively the desktop version:

    $ cmake -B build -DPUZZLES_SDL3=ON
    $ cmake --build build


[PWA]: https://en.wikipedia.org/wiki/Progressive_web_app
[p]: https://www.chiark.greenend.org.uk/~sgtatham/puzzles/
[website]: https://nullprogram.com/tatham-puzzles/
