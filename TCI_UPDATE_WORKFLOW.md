# TCI-Branch aktuell halten

Wenn `drowe67/freedv-gui` (Upstream) aktualisiert wird, kann der eigene
TCI-Branch mit folgenden Schritten nachgezogen werden.

## Voraussetzungen

Die Remotes müssen korrekt eingerichtet sein:

```
origin  = https://github.com/drowe67/freedv-gui.git   (upstream, read-only)
fork    = git@github.com:custosonlinux/freedv-gui.git  (eigener Fork)
tci     = https://github.com/tompatulpan/freedv-tci.git
```

Prüfen mit:
```
git remote -v
```

## Update-Workflow

### 1. Upstream-Änderungen holen

```
git fetch origin
```

### 2. Lokalen master aktualisieren

```
git checkout master
git merge origin/master
```

### 3. TCI-Branch auf neuen master rebasen

```
git checkout feature/tci-integration
git rebase master
```

Der Rebase wendet die TCI-Commits der Reihe nach auf den neuen `master` an.
Bei Konflikten:

```
# Konflikt in Editor lösen, dann:
git add <datei>
git rebase --continue

# Oder einen Commit überspringen (wenn er leer wird):
git rebase --skip

# Oder den Rebase abbrechen und zum Ausgangszustand zurück:
git rebase --abort
```

### 4. Zum eigenen Fork pushen

Da Rebase die Commit-History umschreibt, ist ein Force-Push nötig:

```
git push fork feature/tci-integration --force-with-lease
```

> `--force-with-lease` ist sicherer als `--force`: der Push schlägt fehl,
> wenn zwischenzeitlich jemand anderes etwas in den Branch gepusht hat.

## Tipps

- **Regelmäßig updaten** — je länger gewartet wird, desto mehr Konflikte entstehen.
- Nach dem Rebase einen **Build-Test** machen:
  ```
  cd build_osx && make -j$(sysctl -n hw.logicalcpu)
  ```
- Konflikte in diesen Dateien sind erfahrungsgemäß am häufigsten:
  - `src/main.cpp`
  - `src/ongui.cpp`
  - `src/gui/dialogs/dlg_easy_setup.cpp`
  - `README.md` → immer HEAD-Version bevorzugen
