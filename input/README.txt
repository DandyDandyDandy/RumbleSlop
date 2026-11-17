Put your own files here, then run setup.cmd (one folder up).

  prod.keys             the key file of your own console (Lockpick_RCM writes it)
  the base game         Super Monkey Ball Banana Rumble as .xci (cartridge dump) or .nsp
  the 2.2.0 update      as .nsp
  add-on content        .nsp files, optional: every pack found here is installed

File names do not matter; the setup looks inside the files. Compressed dumps (.nsz, .xcz) are not supported.
After the setup has finished, nothing in this folder is needed any more.

Add-on content bought later: put its .nsp files here together with prod.keys and run  setup.cmd --only dlc
(the base game and the update are not needed for that).

Nothing in this folder is ever committed (see .gitignore). Do not share these files or the built game.
