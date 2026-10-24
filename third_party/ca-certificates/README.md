# CA Certificate Bundle

Mozilla's trusted root CA store, as extracted by the curl project. It replaces
ESP-IDF's built-in bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE` +
`CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`), so the firmware's roots track
Mozilla instead of whichever ESP-IDF release is installed. Used for NTS-KE and
HTTPS OTA.

Source: `https://curl.se/ca/cacert.pem` (see https://curl.se/docs/caextract.html)
Snapshot date: in the file header ("Certificate data from Mozilla as of").
License: Mozilla Public License 2.0 (`MPL-2.0`), per the file header.

Update with:

```sh
make update-certs
```

The download is checked against curl's published SHA-256 and vendored
unmodified.
