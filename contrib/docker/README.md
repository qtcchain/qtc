
# QTC Docker Image (Headless Node)

This Dockerfile builds and runs a **QTC** full node from source.

## Features

* Post-quantum blockchain with MatMul PoW and shielded pool
* Stripped of all non-essential components (tests, debug data, documentation, etc.)
* Data directory persisted via volume
* Accessible via RPC

---

## Build the Docker Image

**make sure you're at the root of the repo first!**

```bash
docker build \
  -f contrib/docker/Dockerfile \
  -t qtcd \
  --load .
```

---

## Run the Node

```bash
docker run -d \
  --init \
  --user $(id -u):$(id -g) \
  --name qtcd \
  -p 19755:19755 -p 127.0.0.1:19754:19754 \
  -v path/to/conf:/etc/qtc/qtc.conf:ro \
  -v path/to/data:/var/lib/qtcd:rw \
  qtcd
```

If your config keeps the legacy RPC default (`8332`) instead of QTC ops
standard (`19334`), change the RPC publish mapping to
`-p 127.0.0.1:8332:8332`.

In case you want to use ZeroMQ sockets, make sure to expose those ports as well by adding `-p host_port:container_port` directives to the command above.
In case `path/to/data` is not writable by your user, consider overriding the `--user` flag.

This will:

* Start the node in the background
* Save the blockchain and config in `/path/to/data`
* Expose peer and RPC ports

---

## Check Node Status

```bash
docker logs qtcd
```

---

## Regtest Cluster Validation

To stage two containerized regtest nodes on an isolated Docker network and run
bidirectional relay/confirmation checks plus bridge view-grant planning,
operator decrypt, settlement submission, mined-transaction grant retrieval, and
recipient shielded-balance verification:

```bash
scripts/m12_docker_regtest_cluster.sh --build-image
```

The harness emits `.qtc-validation/m12-docker-regtest-cluster.json` and removes
only the temporary containers/network/datadirs it creates.

The same gate is available through the focused CI target:

```bash
scripts/ci/run_ci_target.sh bridge-viewgrants
```

---

## Stop the Node

```bash
docker stop qtcd
```

---
