# iFLYTEK product device trees
#
# Each base tree is maintained from its matching factory DTB. Overlays are
# rebuilt from the factory hardware content without decompiler-generated
# __symbols__/__fixups__/__local_fixups__ sections.

dtb-$(CONFIG_ARCH_SPRD) += iclass-c8pro.dtb
dtbo-$(CONFIG_ARCH_SPRD) += iclass-c8pro-overlay.dtbo

dtb-$(CONFIG_ARCH_SPRD) += istudy-c10.dtb
dtbo-$(CONFIG_ARCH_SPRD) += istudy-c10-overlay.dtbo

always := $(dtb-y) $(dtbo-y)
