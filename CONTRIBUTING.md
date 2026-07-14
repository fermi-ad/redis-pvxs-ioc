# Contributing

Issues and pull requests are welcome. Keep changes focused, preserve the
generation-based hot-reload guarantees, and avoid introducing deployment-specific
configuration into the reusable runtime image.

## Start here

1. Clone the public submodules:

   ```sh
   git clone --recurse-submodules https://github.com/fermi-ad/redis-pvxs-ioc.git
   cd redis-pvxs-ioc
   ```

2. Read the [development guide](docs/development.md) and
   [configuration reference](docs/configuration.md).
3. Create a focused branch and include tests or smoke coverage for behavior changes.

## Required validation

At minimum, run the checks relevant to the change:

```sh
docker build --platform linux/amd64 -t redis-pvxs-ioc:local .
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
REDIS_PVXS_IOC_IMAGE=redis-pvxs-ioc:local ./scripts/smoke-test.sh
```

Documentation-only changes should still validate Markdown links, command examples,
and referenced image versions. Do not include secrets, private configuration,
credentials, or production data in issues, commits, or test fixtures.

Pull requests run on GitHub-hosted infrastructure without Fermilab credentials.
Candidate and release publishing remain maintainer-only workflows.
