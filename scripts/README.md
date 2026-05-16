# Dev Scripts

## Relaunch Orion

Use this during UI iteration to rebuild and relaunch the app in one step:

```bash
./scripts/dev-relaunch.sh
```

The script:

1. reconfigures `build-local`
2. rebuilds `Orion`
3. closes the running `Orion` app if it is open
4. launches the freshly built app
