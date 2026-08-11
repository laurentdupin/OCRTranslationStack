# Japanese reading dictionary

The production furigana path expects a compiled MeCab dictionary directory at
`models/japanese/unidic` containing `dicrc`, `sys.dic`, `char.bin` and the
associated matrix files. Run `scripts/setup_models.ps1 -IncludeJapaneseDictionary`
to install the pinned UniDic for Contemporary Written Japanese archive.

`readings.tsv` is intentionally small and exists only for native smoke tests
when the large UniDic asset is not installed. Do not use it as a production
Japanese morphological dictionary.
