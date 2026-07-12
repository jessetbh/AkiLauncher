#pragma once

// Menu sounds: tiny synthesized retro blips (in-memory WAVs via winmm
// PlaySound). Drop assets\sounds\{nav,flip,launch}.wav next to the repo root
// to replace any of them. All calls are fire-and-forget and safe pre-init.
void audio_init();
void play_nav();     // carousel tick
void play_flip();    // box flip / settings open
void play_launch();  // game launch stinger
