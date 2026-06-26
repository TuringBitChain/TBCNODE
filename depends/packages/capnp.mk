package=capnp
$(package)_version=$(native_capnp_version)
$(package)_download_path=$(native_capnp_download_path)
$(package)_download_file=$(native_capnp_download_file)
$(package)_file_name=$(native_capnp_file_name)
$(package)_sha256_hash=$(native_capnp_sha256_hash)

define $(package)_set_vars :=
$(package)_config_opts := -DBUILD_TESTING=OFF -DWITH_OPENSSL=OFF -DWITH_ZLIB=OFF
$(package)_cxxflags += -fdebug-prefix-map=$($(package)_extract_dir)=/usr -fmacro-prefix-map=$($(package)_extract_dir)=/usr
endef

define $(package)_config_cmds
  $($(package)_cmake) .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf lib/pkgconfig
endef
