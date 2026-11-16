# Docker

Full documentation is [on our wiki](https://www.azerothcore.org/wiki/install-with-docker#installation)

## Building

### Prerequisites

Ensure that you have docker, docker compose (v2), and the docker buildx command
installed.

It's all bundled with [Docker Desktop](https://docs.docker.com/get-docker/),
though if you're using Linux you can install them through your distribution's
package manage or by using the [documentation from docker](https://docs.docker.com/engine/install/)

### Running the Build

1. Build containers with command

```console
$ docker compose build
```

    1. Note that the initial build will take a long time, though subsequent builds should be faster

2. Start containers with command

```console
$ docker compose up -d
# Skip the build step
$ docker compose up -d --build
```

    1. Note that this command may take a while the first time, for the database import

3. (on first install) You'll need to attach to the worldserver and create an Admin account

```console
$ docker compose attach ac-worldserver
AC> account create admin password 3 -1
```

## Podman

Rootless podman works with the same files plus `docker-compose.podman.yml`, which maps your host
user to the containers' user so they can write `env/dist/etc` and `env/dist/logs`:

```console
$ systemctl --user enable --now podman.socket
$ podman compose -f docker-compose.yml -f docker-compose.podman.yml up -d --build
$ podman compose -f docker-compose.yml -f docker-compose.podman.yml attach ac-worldserver
```

To skip the `-f` flags, add `COMPOSE_FILE=docker-compose.yml:docker-compose.podman.yml` to `.env`
(that checkout then no longer works with Docker).

- `podman compose` runs docker-compose against `podman.socket`, so both need to be installed.
- The database's healthcheck, which the other services wait for, runs from a systemd user timer.
  Over SSH without a desktop session, enable lingering first (`loginctl enable-linger`), or
  `up` stops with the database reported unhealthy.
