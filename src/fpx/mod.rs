// placeholder
use crate::cpu::FpReg;
pub type Fpx = FpReg;
pub fn default_nan() -> Fpx {
    Fpx {
        mantissa: !0,
        sign_exponent: 0x7FFF,
    }
}
