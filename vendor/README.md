# Vendored build inputs

The three ignored source directories are pinned by
`debian/vendor-sources.lock`:

- `libreoffice-ofd-extension`
- `libreoffice-batch-print`
- `ofdrw`

Run `debian/scripts/fetch-vendor-sources` on a connected preparation host.
`maven-repository` must also be prepared there and included in an offline CI
source submission. See `debian/README.source` for the complete workflow.
`debian/rules` does not download any input.
