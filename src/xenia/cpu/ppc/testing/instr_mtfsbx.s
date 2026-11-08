test_mtfsb1_sets_bit:
  mtfsb1 31
  mffs f1
  blr
  #_ REGISTER_OUT f1 0x0000000000000001

test_mtfsb0_clears_bit:
  mtfsb1 31
  mtfsb0 31
  mffs f1
  blr
  #_ REGISTER_OUT f1 0x0000000000000000
