import type { Metadata, Viewport } from "next";
import { Nunito, Pixelify_Sans } from "next/font/google";
import { SITE_DESCRIPTION, SITE_NAME, SITE_URL } from "@/lib/site";
import "./globals.css";

const pixel = Pixelify_Sans({ subsets: ["latin"], variable: "--font-pixel" });
const body = Nunito({ subsets: ["latin"], variable: "--font-body", preload: false });

export const metadata: Metadata = {
  metadataBase: new URL(SITE_URL),
  title: { default: "Dreamsy · describe a game, then play it", template: "%s · Dreamsy" },
  description: SITE_DESCRIPTION,
  applicationName: SITE_NAME,
  keywords: ["game engine", "open source", "2D games", "2.5D games", "AI game maker", "Luau", "iOS", "Android", "WebAssembly"],
  alternates: { canonical: "/" },
  openGraph: { type: "website", siteName: SITE_NAME, url: "/", title: "Dreamsy · describe a game, then play it", description: SITE_DESCRIPTION, locale: "en_US" },
  twitter: { card: "summary_large_image", title: "Dreamsy · describe a game, then play it", description: SITE_DESCRIPTION },
  appleWebApp: { capable: true, title: SITE_NAME, statusBarStyle: "default" },
  formatDetection: { telephone: false },
};

export const viewport: Viewport = { themeColor: "#1a7ff0", colorScheme: "light" };

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" className={`${pixel.variable} ${body.variable}`}>
      <body>{children}</body>
    </html>
  );
}
