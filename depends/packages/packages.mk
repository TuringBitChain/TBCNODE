packages:=boost openssl libevent zeromq
native_packages := native_ccache

wallet_packages=bdb

upnp_packages=miniupnpc

ipc_packages = capnp
multiprocess_native_packages = native_capnp native_libmultiprocess

darwin_native_packages = native_biplist native_ds_store native_mac_alias

ifneq ($(build_os),darwin)
darwin_native_packages += native_cctools native_cdrkit native_libdmg-hfsplus
endif
