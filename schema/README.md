# CME schema files

Two CME files are needed by the Python decoder and are not stored in this repository:

| File | What it is | Where CME publishes it |
|---|---|---|
| `templates_FixBinary.xml` | MDP 3.0 SBE message templates (schema id 1, version 13) | CME SBE templates (`SBEFix/Production/Templates/` on CME's FTP site) |
| `config.xml` | Multicast channel map (channel → IP:port per feed) | CME MDP 3.0 production configuration (`SBEFix/Production/Configuration/`) |

Put both in this folder, or point `TICKSCOPE_SCHEMA_DIR` at the folder that holds them
(lookup order is in `decoder_py/paths.py`).

The C++ engine does not need them: the two order-book templates it decodes are hand-written in
`engine/include/tickscope/mdp3.hpp`.
