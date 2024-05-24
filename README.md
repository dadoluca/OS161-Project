# OS-project

## Git Repo in Locale su Ubuntu
- installare Git: `sudo apt install git`
- effettuare la clone: `git clone https://github.com/dadoluca/OS-project.git`
- Se viene richiesta l'autenticazione, e non viene accettata quella tramite il proprio account generare un token per autenticarsi:
  1. Andare in git sul proprio profilo "settings"
  2. Barra a sinistra in fondo "Developer settings"
  3. Clicca "Personal access tokens"
  4. Clicca "Generate new token"
  5. Inserisci una descrizione per il token, seleziona le autorizzazioni necessarie (almeno repo per i repository privati)
  6. Clicca su "Generate token".
  7. Copia il token generato e conservalo in un posto sicuro (non sarà più visibile dopo aver lasciato la pagina).
  8. D'ora in poi quando ti autentichi, dopo aver inserito lo username, utilizza questo token per autenticarti
     
## Struttura Cartelle
- **os161-base-2.0.3**: contiene sorgenti, file di configurazione, compilazione ed eseguibili, di os161 e dei tool utilizzati (è quindi l’area in cui si modifica e ri-compila os161)
- **root**: è direttorio in cui eseguire il (fare boot del) sistema operativo, ed eventualmente attivare processi user (è l’area in cui si esegue e si testa il sistema os161).
- **.vscode**: contiene la configurazione di vscode e permette di avere i classici comandi "Build and Install", "Make depend", "Run Config" ecc quando si fa Run Task

## Prova debug e stampa hello
Provare a fare da RunTask:
1. Run Config `HELLO`  (da rieseguire quando cambiamo configurazione)
2. Make Depend `HELLO` (quando vogliamo vengano ricompilate file di dipendenza)
3. Build and Install `HELLO` (da eseguire quando si modifica il codice)
4. Run OS161

Da Run:

5. Start debugging
il debug di vs funziona?

Aprire un terminale e:
1. `cd OS-project`
2. `cd root`
3. `sys161 kernel`
stampa "HELLO" ?

