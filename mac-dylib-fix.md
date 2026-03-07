the dylib copy loop breaks because `ditto` resolves symlinks, renaming e.g.
`libglfw.3.dylib` to `libglfw.3.4.dylib`. the binary still expects `libglfw.3.dylib`
so it fails to load. `cut -w` can also be flaky depending on the runner.

fix: use `cp -L` (dereference but keep the filename) with `basename`, and `awk`
instead of `cut -w`. this also makes the "something more on libs" symlink step
unnecessary.

```yaml
      - name: copy library dependencies
        run: |
          BREW_PREFIX=$(brew --prefix)
          for i in $(seq 1 20); do
            found=0
            for lib in $(find ${APPDIR} -type f \( -name "*.dylib" -o -name "*.so" -o -perm +111 \) -print0 \
              | xargs -0 otool -L 2>/dev/null \
              | awk "/${BREW_PREFIX//\//\\/}/{ gsub(/[[:space:]].*/, \"\", \$1); print \$1 }" \
              | sort -u); do
              name=$(basename "$lib")
              if [ ! -f "${APPDIR}/Contents/Frameworks/${name}" ]; then
                cp -L "$lib" "${APPDIR}/Contents/Frameworks/${name}"
                found=1
              fi
            done
            [ "$found" = "0" ] && break
          done
```

tested locally on arm64, picks up transitive deps (e.g. ffmpeg pulling in
dav1d, x264, x265, openssl) and stabilises after 2-3 iterations.
