# BFCFS Test Fixtures

This directory contains BFC container files used for testing.

## Creating Test Containers

You need to create test BFC containers using the `bfc` tool from https://github.com/zombocoder/bfc:

```bash
# Build and install bfc tool
git clone https://github.com/zombocoder/bfc.git
cd bfc
make
doas make install
cd ..

# Create a simple test container
mkdir test_data
echo "<html><body>Hello World</body></html>" > test_data/index.html
echo "body { color: blue; }" > test_data/style.css
ln -s index.html test_data/link

bfc create test.bfc ./test_data/*

# Copy to fixtures directory
cp test.bfc /path/to/bfcfs-freebsd/tests/fixtures/
```

## Required Test Containers

The test suite expects the following containers:

1. **test.bfc** - Basic container with:
   - `index.html` - Simple HTML file
   - `style.css` - Simple CSS file (optional)
   - `assets/` - Subdirectory with files (used by the `..` and `find` tests)
   - `link` - Symlink to `index.html`
   - `assets/up` - Symlink to `../index.html`

   The bundled `test.bfc` holds a small built web site (`index.html`,
   `vite.svg`, `assets/*`) plus the two symlinks above. To add the symlinks
   to a container of your own:

   ```bash
   ln -s index.html test_data/link
   ln -s ../index.html test_data/assets/up
   (cd test_data && bfc create ../test.bfc *)
   ```

## Container Requirements

- Containers must be valid BFC format with:
  - Valid header and footer
  - Valid index structure
  - At least one file entry
  - No compression/encryption (for basic tests)

## GitHub Actions

The CI workflow will create test containers automatically using the BFC tool
before running the tests.
