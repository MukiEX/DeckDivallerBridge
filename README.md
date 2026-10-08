# Deck Divaller Bridge

A bridge to make the Divaller's "native mode" work in TLAC.dva for PD Loader in Linux.

## The bridge

1. Copy divaller_bridge.py and start.sh to a new folder, `/home/deck/Games/divabridge`
2. Make a venv in the divabridge folder:

```
cd ~/Games/divabridge
python -m venv --copies venv
./venv/bin/pip install pyusb libusb
```

You can test if your Divaller is being picked up by running:

```
./venv/bin/python3 divaller_bridge.py --tui
```

You'll get a display with buttons and slider status.

## The service

Copy divaller_bridge.service to '/home/deck/.config/systemd/user/'

Run:

```
systemctl --user enable divaller_bridge
systemctl --user start divaller_bridge
```

And then to check its status:

```
systemctl --user status divaller_bridge
journalctl -u divaller_bridge -n 90 -f
```

If the service is running and you're on the latest (`PD-Loader-Release-AppVeyor-becac44e` or later) version of TLAC.dva, your Divaller should now behave identically to how it does in Windows.
