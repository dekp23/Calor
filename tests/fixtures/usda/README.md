# USDA fixture

Small FoodData Central style CSVs for the importer tests.

- fdc_id 167959 (Snacks, popcorn, air-popped) uses real SR Legacy values per 100 g from
  [FoodData Central](https://fdc.nal.usda.gov/portal-data/external/167959).
- fdc_ids 9000001 to 9000005 are synthetic rows for edge cases: Atwater-only energy,
  an excluded data type, a food with no energy, a quoted name with a comma and line
  break, a blank amount, and a branded food.
- food.csv has a UTF-8 byte-order mark and CRLF line endings on purpose.
