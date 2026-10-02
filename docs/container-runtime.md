# Container runtime and secret files

The runtime image runs as UID/GID `10001:10001` (`ioc`). It needs no root privileges
for the default PVA/RecCeiver ports, writes logs to stdout/stderr, and supports a
read-only root filesystem. Configurations, ACFs and credential files must be
readable by that identity. The entrypoint does not change ownership, elevate
privileges or copy secrets into environment variables.

Use Docker's `--user UID:GID` or Compose's `user: "UID:GID"` when deployment-owned
files use another identity. For example, after setting `REDIS_PVXS_IOC_IMAGE` to
the candidate or qualified image being tested:

```sh
docker run --rm --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --user 1000:1000 \
  --mount type=bind,source=/srv/ioc/config,target=/config,readonly \
  --mount type=bind,source=/srv/ioc/secrets,target=/run/secrets,readonly \
  "$REDIS_PVXS_IOC_IMAGE" --check-config /config/ioc.yaml
```

Choose the UID/GID that owns or can read the mounted files; `1000:1000` is only an
example. For a live IOC, use `--config` and the site's documented
[PVA network settings](pva-networking.md). Configuration directories can be mounted
read-only to the container while operators replace files on the host. This also
allows the ACF watcher to observe atomic policy replacement. Mount directories
when files will be rotated: a single-file bind mount may keep the old inode.

## Redis credentials

```yaml
redis:
  host: redis
  port: 6379
  base_key: beam
  user_file: /run/secrets/redis_user
  password_file: /run/secrets/redis_password
```

The same fields work per entry in `redis_backends`. Relative file paths are
resolved beside the YAML file, including during offline validation. Inline
`user`/`password` remain supported, but each value and its `_file` alternative
are mutually exclusive.

Secret files must be regular text files, at most 16 KiB excluding one optional
trailing LF/CRLF. Empty files, embedded newlines/NUL, special files, and unreadable
paths fail configuration parsing without displaying contents. Spaces are preserved.
Symlinks to regular files are supported for projected secrets. The process keeps
the loaded credential in memory for backend authentication.

Rotate credentials with an atomic file replacement and configuration reload.
Changes are detected against the loaded configuration; a parse failure preserves
the previous generation. Coordinate the Redis ACL/password change with the IOC
reload. An offline diff reads the secret files as they exist at comparison time.

## ChannelFinder credentials

Set `CHANNELFINDER_USERNAME_FILE` and `CHANNELFINDER_PASSWORD_FILE` to mounted
paths for the one-shot publisher. Nonempty inline variables and their file
alternatives are mutually exclusive. Empty inline variables count as unset.
For example:

```sh
docker run --rm --read-only --cap-drop=ALL \
  --mount type=bind,source=/srv/ioc/config,target=/config,readonly \
  --mount type=bind,source=/srv/ioc/secrets,target=/run/secrets,readonly \
  -e CHANNELFINDER_USERNAME_FILE=/run/secrets/channelfinder_user \
  -e CHANNELFINDER_PASSWORD_FILE=/run/secrets/channelfinder_password \
  --entrypoint /opt/redis-pvxs-ioc/bin/redis-pvxs-channelfinder-sync \
  "$REDIS_PVXS_IOC_IMAGE" --config /config/ioc.yaml
```

The Compose publisher also forwards the `_FILE` variables; add corresponding
read-only mounts in a deployment-owned override. Use the final ChannelFinder URL:
authenticated redirects remain prohibited. `--dry-run` does not read ChannelFinder
credential inputs or send requests.

Keep deployment secrets outside the build context and source tree. The supplied
`.dockerignore` additionally excludes `.env`, `.env.*`, `.secrets/`, `secrets/` and
`*.secret` files. These patterns do not identify arbitrary credential filenames.

## Access policy

[The restrictive configuration](../demo/config.access.restrictive.yaml) uses an
ACF with denied default access and separate observer/operator/admin identities.
Replace its placeholder users and assign each writable endpoint deliberately.
The runtime UID controls filesystem access; the ACF controls PVA client operations.
They serve different purposes and should both match the deployment's ownership.
