set(SC_TEST_LINK_LIBRARIES sc-s3)

# Signing against AWS's published examples, sizes, checksums, XML, settings and part sizes.
add_sc_test(units LABELS unit TIMEOUT 60)
# The client against an S3-compatible server when SC_S3_TEST_ENDPOINT names one (MinIO: http://127.0.0.1:9000), or AWS
# when it is "aws", with AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY; skipped otherwise.
add_sc_test(server LABELS integration TIMEOUT 600)
