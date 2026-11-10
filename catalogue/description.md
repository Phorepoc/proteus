Proteus lets the PC edition of Phorepoc OS start apps built for ARM phones. Once it is installed, an ARM app installs from
its SIS like any other, and the system hands it to Proteus when it starts. Proteus translates the app's ARM code to the
PC's own with the dynarmic JIT, and the app keeps its own identity and the capabilities it declared.

This is an early release. For now an app also needs the ARM system libraries it imports staged on the disk; running ARM
apps directly against the system's own libraries is the next step. See the compatibility list in the repository.
