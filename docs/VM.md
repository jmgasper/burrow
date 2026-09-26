# Burrow's Haiku R1/beta6 test VM

| Item | Value |
| --- | --- |
| Guest | Haiku R1~beta6 hrev59866+79, x86_64 |
| VNC | `127.0.0.1:5908` (display `:8`), no password, loopback only |
| SSH | `127.0.0.1:2228`, user `user`, key `.vm/id_ed25519` (`bash tools/haiku.sh`) |
| QMP | `.vm/qmp.sock` (`python3 tools/vm.py status|screenshot|click|drag|type|key`) |
| Resources | 6 vCPUs, 6 GiB RAM (`BURROW_VM_CPUS`, `BURROW_VM_MEMORY`) |
| Source checkout in the guest | `/boot/home/burrow` |
| Test profiles | `~/Desktop/downloaded-client-config.ovpn` (SAML), `~/Desktop/fake-mtls.ovpn` |

The workspace drive had no room for another 20 GB disk clone. So `tools/run-vm.sh`
boots **TasAmp's** beta6 disk (`../tasamp/.vm/work.qcow2`, override with
`BURROW_VM_BASE`) read-only, through a throwaway qcow2 overlay in `BURROW_VM_TMPDIR`
(default `/mnt/HaikuWork/tmp/burrow-vm`; it needs 3 GiB free).

- The overlay is recreated whenever the base disk has changed. Then run
  `bash tools/provision-vm.sh` (packages, `python3` link, crash reports) and
  `bash tools/sync-build.sh` again.
- While Burrow's VM runs, TasAmp's VM cannot start (QEMU locks the base disk). Stop one
  VM before starting the other.
- The SSH key is TasAmp's (copied into `.vm/`), because the guest is TasAmp's disk.

## Everyday commands

```sh
bash tools/run-vm.sh                    # start
bash tools/provision-vm.sh              # after a fresh overlay
bash tools/sync-build.sh                # sync sources, build, print errors
bash tools/sync-build.sh check          # core tests in Haiku
bash tools/haiku.sh 'cd /boot/home/burrow && sh openvpn/build.sh'
bash tools/haiku.sh 'cd /boot/home/burrow && nohup build-haiku/Burrow >/tmp/burrow.out 2>&1 </dev/null &'
bash tools/haiku.sh 'hey application/x-vnd.Burrow quit'
python3 tools/vm.py screenshot          # .vm/screen.png
bash tools/haiku.sh 'shutdown -q'
```

The guest reaches the host as `10.0.2.2`. `bash tools/fake-endpoint.sh start [saml|mtls]
[split|full]` serves the stand-in AWS endpoint there, and `bash tools/fake-endpoint.sh
profile saml udp` prints a matching profile.

Haiku's `ps` puts the team name first: `ps | grep "[b]urrow-openvpn" | awk '{print $(NF-3)}'`
gives the team id.
